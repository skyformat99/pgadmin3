//////////////////////////////////////////////////////////////////////////
//
// pgAdmin III - PostgreSQL Tools
// RCS-ID:      $Id$
// Copyright (C) 2002 - 2010, The pgAdmin Development Team
// This software is released under the Artistic Licence
//
// pgConnBase.cpp - PostgreSQL basic Connection class
//
/////////////////////////////////////////////////////////////////////////

// wxWindows headers
#include <wx/wx.h>

// PostgreSQL headers
#include <libpq-fe.h>

// Network  headers
#ifdef __WXMSW__
#include <winsock.h>
#else

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>

#ifndef INADDR_NONE
#define INADDR_NONE (-1)
#endif

#endif

// App headers
#include "base/pgConnBase.h"
#include "base/base.h"
#include "base/sysLogger.h"


double pgConnBase::libpqVersion=8.0;


void pgConnBase::ExamineLibpqVersion()
{
    libpqVersion=7.3;
    PQconninfoOption *cio=PQconndefaults();

    if (cio)
    {
        PQconninfoOption *co=cio;
        while (co->keyword)
        {
            if (!strcmp(co->keyword, "sslmode"))
            {
                libpqVersion=7.4;
                break;
            }
            co++;
        }
        PQconninfoFree(cio);
    }
}


static void pgNoticeProcessor(void *arg, const char *message)
{
    ((pgConnBase*)arg)->Notice(message);
}


pgConnBase::pgConnBase(const wxString& server, const wxString& database, const wxString& username, const wxString& password, int port, int sslmode, OID oid)
{
    wxLogInfo(wxT("Creating pgConnBase object"));
    wxString msg, hostip, hostname;

    conv = &wxConvLibc;
    needColQuoting = false;
    utfConnectString = false;

    // Check the hostname/ipaddress
    struct hostent *host;
    unsigned long addr;
    conn=0;
    noticeArg=0;
    connStatus = PGCONN_BAD;
    dbname = database;
    
#ifdef __WXMSW__
    struct in_addr ipaddr;
#else
    unsigned long ipaddr;
#endif
    
#ifndef __WXMSW__
    if (!(server.IsEmpty() || server.StartsWith(wxT("/"))))
    {
#endif
        addr = inet_addr(server.ToAscii());
        if (addr == INADDR_NONE) // szServer is not an IP address
        {
            host = gethostbyname(server.ToAscii());
            if (host == NULL)
            {
                connStatus = PGCONN_DNSERR;
                wxLogError(__("Could not resolve hostname %s"), server.c_str());
                return;
            }

        	memcpy(&(ipaddr),host->h_addr,host->h_length); 
	    	hostip = wxString::FromAscii(inet_ntoa(*((struct in_addr*) host->h_addr_list[0])));
            hostname = server;
        }
    	else
        {
    	    hostip = server;
            hostname = server;
        }
#ifndef __WXMSW__
    }
    else
        hostname = server;
#endif
    

    wxLogInfo(wxT("Server name: %s (resolved to: %s)"), server.c_str(), hostip.c_str());

    // Create the connection string
    wxString connstr;
    if (!hostname.IsEmpty()) {
      connstr.Append(wxT(" host="));
      connstr.Append(qtConnString(hostname));
    }
    if (!hostip.IsEmpty()) {
      connstr.Append(wxT(" hostaddr="));
      connstr.Append(hostip);
    }
    if (!database.IsEmpty()) {
      connstr.Append(wxT(" dbname="));
      connstr.Append(qtConnString(database));
    }
    if (!username.IsEmpty()) {
      connstr.Append(wxT(" user="));
      connstr.Append(qtConnString(username));
    }
    if (!password.IsEmpty()) {
      connstr.Append(wxT(" password="));
      connstr.Append(qtConnString(password));
    }

    if (port > 0) {
      connstr.Append(wxT(" port="));
      connstr.Append(NumToStr((long)port));
    }

    if (libpqVersion > 7.3)
    {
        switch (sslmode)
        {
            case 1: connstr.Append(wxT(" sslmode=require"));   break;
            case 2: connstr.Append(wxT(" sslmode=prefer"));    break;
            case 3: connstr.Append(wxT(" sslmode=allow"));     break;
            case 4: connstr.Append(wxT(" sslmode=disable"));   break;
        }
    }
    else
    {
        switch (sslmode)
        {
            case 1: connstr.Append(wxT(" requiressl=1"));   break;
            case 2: connstr.Append(wxT(" requiressl=0"));   break;
        }
    }
    connstr.Trim(false);
    
    // Open the connection
    wxString cleanConnStr = connstr;
    cleanConnStr.Replace(qtConnString(password), wxT("'XXXXXX'"));
    wxLogInfo(wxT("Opening connection with connection string: %s"), cleanConnStr.c_str());

#if wxUSE_UNICODE
    wxCharBuffer cstrUTF=connstr.mb_str(wxConvUTF8);
    conn = PQconnectdb(cstrUTF);
    if (PQstatus(conn) == CONNECTION_OK)
        utfConnectString = true;
    else
    {
        wxCharBuffer cstrLibc=connstr.mb_str(wxConvLibc);
        if (strcmp(cstrUTF, cstrLibc))
        {
            PQfinish(conn);
            conn = PQconnectdb(cstrLibc);
        }
    }
#else
    conn = PQconnectdb(connstr.ToAscii());
#endif

    dbHost = server;

    // Set client encoding to Unicode/Ascii
    if (PQstatus(conn) == CONNECTION_OK)
    {
        connStatus = PGCONN_OK;
        PQsetNoticeProcessor(conn, pgNoticeProcessor, this);


        wxString sql=wxT("SET DateStyle=ISO;SELECT oid, pg_encoding_to_char(encoding) AS encoding, datlastsysoid\n")
                      wxT("  FROM pg_database WHERE ");

        if (oid)
            sql += wxT("oid = ") + NumToStr(oid);
        else
		{
			// Note, can't use qtDbString here as we don't know the server version yet.
			wxString db = database;
			db.Replace(wxT("\\"), wxT("\\\\"));
			db.Replace(wxT("'"), wxT("''"));
            sql += wxT("datname='") + database + wxT("'");
		}
		

        pgSetBase *set = ExecuteSet(sql);


        if (set)
        {
            if (set->ColNumber(wxT("\"datlastsysoid\"")) >= 0)
                needColQuoting = true;

            lastSystemOID = set->GetOid(wxT("datlastsysoid"));
            dbOid = set->GetOid(wxT("oid"));
            wxString encoding = set->GetVal(wxT("encoding"));

#if wxUSE_UNICODE
            if (encoding != wxT("SQL_ASCII") && encoding != wxT("MULE_INTERNAL"))
            {
                encoding = wxT("UNICODE");
                conv = &wxConvUTF8;
            }
            else
                conv = &wxConvLibc;
#endif

            wxLogInfo(wxT("Setting client_encoding to '%s'"), encoding.c_str());
            if (PQsetClientEncoding(conn, encoding.ToAscii()))
				wxLogError(wxT("%s"), GetLastError().c_str());

            delete set;
        }
    }
}


pgConnBase::~pgConnBase()
{
    wxLogInfo(wxT("Destroying pgConnBase object"));
    Close();
}



void pgConnBase::Close()
{
    if (conn)
        PQfinish(conn);
    conn=0;
    connStatus=PGCONN_BAD;
}


wxString pgConnBase::GetName() const
{
    wxString str;
    if (dbHost.IsEmpty())
        str.Printf(_("%s on local socket"), dbname.c_str());
    else
        str.Printf(_("%s on %s:%d"), dbname.c_str(), dbHost.c_str(), GetPort());
    return str;
}

#ifdef SSL
// we don't define USE_SSL so we don't get ssl.h included
extern "C"
{
extern void *PQgetssl(PGconn *conn);
}

bool pgConnBase::IsSSLconnected()
{
    return (conn && PQstatus(conn) == CONNECTION_OK && PQgetssl(conn) != NULL);
}
#else

bool pgConnBase::IsSSLconnected()
{
    return false ;
}
#endif


void pgConnBase::RegisterNoticeProcessor(PQnoticeProcessor proc, void *arg)
{
    noticeArg=arg;
    noticeProc=proc;
}




void pgConnBase::Notice(const char *msg)
{
    wxString str(msg, *conv);
    wxLogNotice(wxT("%s"), str.c_str());

    if (noticeArg && noticeProc)
        (*noticeProc)(noticeArg, msg);
}


pgNotification *pgConnBase::GetNotification()
{
    pgNotify *notify;

    notify = PQnotifies(conn);
    if (!notify)
        return NULL;

    pgNotification *ret = new pgNotification;
    ret->name = wxString(notify->relname, *conv);
    ret->pid = notify->be_pid;
    ret->data = wxString(notify->extra, *conv);

    return ret;
}

int pgConnBase::GetTxStatus()
{
    return PQtransactionStatus(conn);
}

//////////////////////////////////////////////////////////////////////////
// Execute SQL
//////////////////////////////////////////////////////////////////////////

bool pgConnBase::ExecuteVoid(const wxString& sql)
{
    if (GetStatus() != PGCONN_OK)
        return false;

    // Execute the query and get the status.
    PGresult *qryRes;

    wxLogSql(wxT("Void query (%s:%d): %s"), this->GetHost().c_str(), this->GetPort(), sql.c_str());
    qryRes = PQexec(conn, sql.mb_str(*conv));
    lastResultStatus = PQresultStatus(qryRes);
    SetLastResultError(qryRes);

    // Check for errors
    if (lastResultStatus != PGRES_TUPLES_OK &&
        lastResultStatus != PGRES_COMMAND_OK)
    {
        LogError();
        return false;
    }

    // Cleanup & exit
    PQclear(qryRes);
    return  true;
}



wxString pgConnBase::ExecuteScalar(const wxString& sql)
{
    wxString result;

    if (GetStatus() == PGCONN_OK)
    {
        // Execute the query and get the status.
        PGresult *qryRes;
        wxLogSql(wxT("Scalar query (%s:%d): %s"), this->GetHost().c_str(), this->GetPort(), sql.c_str());
        qryRes = PQexec(conn, sql.mb_str(*conv));
        lastResultStatus = PQresultStatus(qryRes);
        SetLastResultError(qryRes);
        
        // Check for errors
        if (lastResultStatus != PGRES_TUPLES_OK && lastResultStatus != PGRES_COMMAND_OK)
        {
            LogError();
            PQclear(qryRes);
            return wxEmptyString;
        }

	    // Check for a returned row
        if (PQntuples(qryRes) < 1)
        {
		    wxLogInfo(wxT("Query returned no tuples"));
            PQclear(qryRes);
            return wxEmptyString;
	    }
	    
	    // Retrieve the query result and return it.
        result=wxString(PQgetvalue(qryRes, 0, 0), *conv);

        wxLogSql(wxT("Query result: %s"), result.c_str());

        // Cleanup & exit
        PQclear(qryRes);
    }

    return result;
}

pgSetBase *pgConnBase::ExecuteSet(const wxString& sql)
{
    // Execute the query and get the status.
    if (GetStatus() == PGCONN_OK)
    {
        PGresult *qryRes;
        wxLogSql(wxT("Set query (%s:%d): %s"), this->GetHost().c_str(), this->GetPort(), sql.c_str());
        qryRes = PQexec(conn, sql.mb_str(*conv));

        lastResultStatus= PQresultStatus(qryRes);
        SetLastResultError(qryRes);

        if (lastResultStatus == PGRES_TUPLES_OK || lastResultStatus == PGRES_COMMAND_OK)
        {
            pgSetBase *set = new pgSetBase(qryRes, this, *conv, needColQuoting);
            if (!set)
            {
                wxLogError(__("Couldn't create a pgSet object!"));
                PQclear(qryRes);
            }
    	    return set;
        }
        else
        {
            LogError();
            PQclear(qryRes);
        }
    }
    return 0;
}

//////////////////////////////////////////////////////////////////////////
// Info
//////////////////////////////////////////////////////////////////////////

wxString pgConnBase::GetLastError() const
{ 
    wxString errmsg;
	char *pqErr;
    if (conn && (pqErr = PQerrorMessage(conn)) != 0)
    {
        errmsg=wxString(pqErr, wxConvUTF8);
        if (errmsg.IsNull())
            errmsg=wxString(pqErr, wxConvLibc);
    }
    else
    {
        if (connStatus == PGCONN_BROKEN)
            errmsg = _("Connection to database broken.");
        else
            errmsg = _("No connection to database.");
    }
    return errmsg;
}



void pgConnBase::LogError()
{
    if (conn)
    {
        wxLogError(wxT("%s"), GetLastError().c_str());

        IsAlive();
#if 0
        ConnStatusType status = PQstatus(conn);
        if (status == CONNECTION_BAD)
        {
            PQfinish(conn);
            conn=0;
            connStatus = PGCONN_BROKEN;
        }
#endif
    }
}



bool pgConnBase::IsAlive()
{
    if (GetStatus() != PGCONN_OK)
    {
        lastResultError.severity = wxString(wxT("FATAL"));
        lastResultError.msg_primary = wxString(PQerrorMessage(conn), *conv);
        lastResultError.formatted_msg = lastResultError.severity + wxT(": ") + lastResultError.msg_primary;
        wxLogError(lastResultError.msg_primary);
        return false;
    }

    PGresult *qryRes = PQexec(conn, "SELECT 1;");
    lastResultStatus = PQresultStatus(qryRes);
    if (lastResultStatus != PGRES_TUPLES_OK)
    {
        PQclear(qryRes);
        qryRes = PQexec(conn, "ROLLBACK TRANSACTION; SELECT 1;");
        lastResultStatus = PQresultStatus(qryRes);
        SetLastResultError(qryRes);
    }
    PQclear(qryRes);

    // Check for errors
    if (lastResultStatus != PGRES_TUPLES_OK)
    {
        PQfinish(conn);
        conn=0;
        connStatus = PGCONN_BROKEN;
        return false;
    }

    return true;
}


int pgConnBase::GetStatus() const
{
    if (!this)
        return PGCONN_BAD;

    if (conn)
        ((pgConnBase*)this)->connStatus = PQstatus(conn);

    return connStatus;
}


wxString pgConnBase::GetVersionString()
{
	return ExecuteScalar(wxT("SELECT version();"));
}

void pgConnBase::SetLastResultError(PGresult *res)
{
    lastResultError.severity = wxString(PQresultErrorField(res, PG_DIAG_SEVERITY), *conv);
    lastResultError.sql_state = wxString(PQresultErrorField(res, PG_DIAG_SQLSTATE), *conv);
    lastResultError.msg_primary = wxString(PQresultErrorField(res, PG_DIAG_MESSAGE_PRIMARY), *conv);
    lastResultError.msg_detail = wxString(PQresultErrorField(res, PG_DIAG_MESSAGE_DETAIL), *conv);
    lastResultError.msg_hint = wxString(PQresultErrorField(res, PG_DIAG_MESSAGE_HINT), *conv);
    lastResultError.statement_pos = wxString(PQresultErrorField(res, PG_DIAG_STATEMENT_POSITION), *conv);
    lastResultError.internal_pos = wxString(PQresultErrorField(res, PG_DIAG_INTERNAL_POSITION), *conv);
    lastResultError.internal_query = wxString(PQresultErrorField(res, PG_DIAG_INTERNAL_QUERY), *conv);
    lastResultError.context = wxString(PQresultErrorField(res, PG_DIAG_CONTEXT), *conv);
    lastResultError.source_file = wxString(PQresultErrorField(res, PG_DIAG_SOURCE_FILE), *conv);
    lastResultError.source_line = wxString(PQresultErrorField(res, PG_DIAG_SOURCE_LINE), *conv);
    lastResultError.source_function = wxString(PQresultErrorField(res, PG_DIAG_SOURCE_FUNCTION), *conv);

    wxString errMsg = lastResultError.severity + wxT(": ") + lastResultError.msg_primary;

    if (!lastResultError.sql_state.IsEmpty())
    {
        errMsg += wxT("\n");
        errMsg += _("SQL state: ");
        errMsg += lastResultError.sql_state;
    }

    if (!lastResultError.msg_detail.IsEmpty())
    {
        errMsg += wxT("\n");
        errMsg += _("Detail: ");
        errMsg += lastResultError.msg_detail;
    }

    if (!lastResultError.msg_hint.IsEmpty())
    {
        errMsg += wxT("\n");
        errMsg += _("Hint: ");
        errMsg += lastResultError.msg_hint;
    }

    if (!lastResultError.statement_pos.IsEmpty())
    {
        errMsg += wxT("\n");
        errMsg += _("Character: ");
        errMsg += lastResultError.statement_pos;
    }

    if (!lastResultError.context.IsEmpty())
    {
        errMsg += wxT("\n");
        errMsg += _("Context: ");
        errMsg += lastResultError.context;
    }
    lastResultError.formatted_msg = errMsg;
}