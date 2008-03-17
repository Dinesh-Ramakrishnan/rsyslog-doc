/* The RELP (reliable event logging protocol) core protocol library.
 *
 * Copyright 2008 by Rainer Gerhards and Adiscon GmbH.
 *
 * This file is part of librelp.
 *
 * Librelp is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Librelp is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Librelp.  If not, see <http://www.gnu.org/licenses/>.
 *
 * A copy of the GPL can be found in the file "COPYING" in this distribution.
 *
 * If the terms of the GPL are unsuitable for your needs, you may obtain
 * a commercial license from Adiscon. Contact sales@adiscon.com for further
 * details.
 *
 * ALL CONTRIBUTORS PLEASE NOTE that by sending contributions, you assign
 * your copyright to Adiscon GmbH, Germany. This is necessary to permit the
 * dual-licensing set forth here. Our apologies for this inconvenience, but
 * we sincerely believe that the dual-licensing model helps us provide great
 * free software while at the same time obtaining some funding for further
 * development.
 */
#include "config.h"
#include <stdlib.h>
#include <sys/select.h>
#include <assert.h>
#include "relp.h"
#include "relpsrv.h"
#include "relpframe.h"
#include "relpsess.h"
#include "dbllinklist.h"


/* DESCRIPTION OF THE RELP PROTOCOL
 *
 * Relp uses a client-server model with fixed roles. The initiating part of the
 * connection is called the client, the listening part the server. In the state
 * diagrams below, C stand for client and S for server.
 *
 * Relp employs a command-response model, that is the client issues command to
 * which the server responds. To facilitate full-duplex communication, multiple
 * commands can be issued at the same time, thus multiple responses may be 
 * outstanding at a given time. The server may reply in any order. To conserve
 * ressources, the number of outstanding commands is limited by a window. Each
 * command is assigned a (relative) unique, monotonically increasing ID. Each
 * response must include that ID. New commands may only be issued if the new ID
 * is less than the oldes unresponded ID plus the window size. So a connection
 * stalls if the server does not respond to all requests. (TODO: evaluate if this
 * needs to be a hard requirement or if it is sufficient to just allow "n" outstanding
 * commands at a time).
 *
 * A command and its response is called a relp transaction.
 *
 * If something goes really wrong, both the client and the server may terminate
 * the TCP connection at any time. Any outstanding commands are considered to
 * have been unsuccessful in this case.
 *
 * SENDING MESSAGES
 * Because it is so important, I'd like to point it specifically out: sending a
 * message is "just" another RELP command. The reply to that command is the ACK/NAK
 * for the message. So every message *is* acknowledged. RELP options indicate how
 * "deep" this acknowledge is (one we have implemented that), in the most extreme
 * case a RELP client may ask a RELP server to ack only after the message has been
 * completely acted upon (e.g. successfully written to database) - but this is far
 * away in the future. For now, keep in mind that message loss will always be detected
 * because we have app-level acknowledgement.
 *
 * RELP FRAME
 * All relp transaction are carried out over a consistent framing.
 *
 * RELP-FRAME     = HEADER DATA TRAILER
 * DATA           = *OCTET ; command-defined data
 * HEADER         = TXNR SP COMMAND SP DATALEN
 * TXNR           = NUMBER ; relp transaction number, monotonically increases
 * DATALEN        = NUMBER
 * #old:COMMAND        = "init" / "go" / "msg" / "close" / "rsp" / "abort" ; max length = 32
 * COMMAND        = 1*32ALPHA
 * TRAILER        = LF ; to detect framing errors and enhance human readibility
 * ALPHA          = letter ; ('a'..'z', 'A'..'Z')
 * NUMBER         = 1*9DIGIT
 * DIGIT          = %d48-57
 * LF             = %d10
 * SP             = %d32
 *
 * RSP DATA CONTENT:
 * RSP-HEADER     = RSP-CODE [SP HUMANMSG] LF [CMDDATA]
 * RSP-CODE       = 200 / 500 ; 200 is ok, all the rest currently erros
 * HUAMANMSG      = *OCTET ; a human-readble message without LF in it
 * CMDDATA        = *OCTET ; semantics depend on original command
 *
 * DATALEN is the number of octets in DATA (so the frame length excluding the length
 * of HEADER and TRAILER).
 *
 * Note that TXNR montonically increases, but at some point latches. The requirement
 * is to have enough different number values to handle a complete window. This may be
 * used to optimize traffic a bit by using short numbers. E.g. transaction numbers
 * may (may!) be latched at 1000 (so the next TXNR after 999 will be 0).
 *
 *
 * COMMAND SEMANTICS
 *
 *
 * Command "rsp"
 * Response to a client-issued command. The TXNR MUST match the client's command
 * TXN. The data part contains RSP-HEADER as defined above. It is a response code,
 * optionally followed by a space and additional data (depending on the client's command).
 *   Return state values are: 200 - OK, 500 - error
 *
 *
 * Command "init"
 * Intializes a connection to the server. May include offers inside the data. Offers
 * provide information about services supported by the client.
 *
 * When the server receives an init, it parses the offers, checks what it itself supports
 * and provides those offers that it accepts in the "rsp". 
 *
 * When the client receives the "rsp", it checks the servers offers and finally selects
 * those that should be used during the session. Please note that this doesn't imply the
 * client selects e.g. security strength. To require a specific security strength, the
 * server must be configured to offer only those options back to the client that it is
 * happy to accept. So the client can only select from those. As such, even though the
 * client makes the final feature selection, the server is dictating what needs to be 
 * used.
 *
 * Once the client has made its selection, it sends back a "go" command to the server,
 * which finally initializes the connection and makes it ready for transmission of other
 * commands. Note that the connection is only ready AFTER the server has sent a positive
 * response to the "go" command, so the client must wait for it (and a negative response
 * means the connection is NOT usable).
 *
 *
 * OFFERS
 *
 * During session setup, "offers" are exchange between client and server. An "offer" describes
 * a specific feature or operation mode. Always present must be the "relp_version" offer which
 * tells the other side which version of relp is in use.
 *
 * ABNF for offer strings
 *
 * OFFER       = FEATURENAME [= VALUE] LF
 * FEATURENAME = *32OCTET
 * VALUE       = *255OCTET
 *
 * Currently defined values:
 * FEATURENAME             VALUE
 * relp_version            1 (this specification)
 *
 *
 * STATE DIAGRAMS
 * ... detailling some communications scenarios:
 *
 * Session Startup:
 * C                                          S
 * cmd: "init", data: offer          -----> (selects supported offers)
 * (selects offers to use)           <----- cmd: "rsp", data "accepted offers"
 * cmd: "go", data: "offers to use"   -----> (initializes connection)
 *                                   <----- cmd: "rsp", data "200 OK" (or error)
 *
 *                 ... transmission channel is ready to use ....
 *
 * Message Transmission
 * C                                          S
 * cmd: "msg", data: msgtext         -----> (processes message)
 * (indicates msg as processed)      <----- cmd: "rsp", data OK/Error
 *
 * Session Termination
 * C                                          S
 * cmd: "close", data: none?         -----> (processes termination request)
 * (terminates session)              <----- cmd: "rsp", data OK/Error
 *                                          (terminates session)
 */

/* ------------------------------ some internal functions ------------------------------ */

/* add an entry to our server list. The server object is handed over and must
 * no longer be accessed by the caller.
 * rgerhards, 2008-03-17
 */
static relpRetVal
relpEngineAddToSrvList(relpEngine_t *pThis, relpSrv_t *pSrv)
{
	relpEngSrvLst_t *pSrvLstEntry;

	ENTER_RELPFUNC;
	RELPOBJ_assert(pThis, Engine);
	RELPOBJ_assert(pSrv, Srv);

	if((pSrvLstEntry = calloc(1, sizeof(relpEngSrvLst_t))) == NULL)
		ABORT_FINALIZE(RELP_RET_OUT_OF_MEMORY);

	pSrvLstEntry->pSrv = pSrv;

	pthread_mutex_lock(&pThis->mutSrvLst);
	DLL_Add(pSrvLstEntry, pThis->pSrvLstRoot, pThis->pSrvLstLast);
	++pThis->lenSrvLst;
	pthread_mutex_unlock(&pThis->mutSrvLst);

finalize_it:
	LEAVE_RELPFUNC;
}


/* add an entry to our session list. The session object is handed over and must
 * no longer be accessed by the caller.
 * rgerhards, 2008-03-17
 */
static relpRetVal
relpEngineAddToSess(relpEngine_t *pThis, relpSess_t *pSess)
{
	relpEngSessLst_t *pSessLstEntry;

	ENTER_RELPFUNC;
	RELPOBJ_assert(pThis, Engine);
	RELPOBJ_assert(pSess, Sess);

	if((pSessLstEntry = calloc(1, sizeof(relpEngSessLst_t))) == NULL)
		ABORT_FINALIZE(RELP_RET_OUT_OF_MEMORY);

	pSessLstEntry->pSess = pSess;

	pthread_mutex_lock(&pThis->mutSessLst);
	DLL_Add(pSessLstEntry, pThis->pSessLstRoot, pThis->pSessLstLast);
	++pThis->lenSessLst;
	pthread_mutex_unlock(&pThis->mutSessLst);

finalize_it:
	LEAVE_RELPFUNC;
}


/* Delete an entry from our session list. The session object is destructed.
 * rgerhards, 2008-03-17
 */
static relpRetVal
relpEngineDelSess(relpEngine_t *pThis, relpEngSessLst_t *pSessLstEntry)
{
	ENTER_RELPFUNC;
	RELPOBJ_assert(pThis, Engine);
	assert(pSessLstEntry != NULL);

	pthread_mutex_lock(&pThis->mutSessLst);
	DLL_Del(pSessLstEntry, pThis->pSessLstRoot, pThis->pSessLstLast);
	--pThis->lenSessLst;
	pthread_mutex_unlock(&pThis->mutSessLst);

	relpSessDestruct(&pSessLstEntry->pSess);
	free(pSessLstEntry);

finalize_it:
	LEAVE_RELPFUNC;
}

/* ------------------------------ end of internal functions ------------------------------ */

/** Construct a RELP engine instance
 * This is the first thing that a caller must do before calling any
 * RELP function. The relp engine must only destructed after all RELP
 * operations have been finished.
 */
relpRetVal
relpEngineConstruct(relpEngine_t **ppThis)
{
	relpEngine_t *pThis;

	ENTER_RELPFUNC;
	assert(ppThis != NULL);
	if((pThis = calloc(1, sizeof(relpEngine_t))) == NULL) {
		ABORT_FINALIZE(RELP_RET_OUT_OF_MEMORY);
	}

	RELP_CORE_CONSTRUCTOR(pThis, Engine);
	pthread_mutex_init(&pThis->mutSrvLst, NULL);
	pthread_mutex_init(&pThis->mutSessLst, NULL);

	*ppThis = pThis;

finalize_it:
	LEAVE_RELPFUNC;
}


/** Destruct a RELP engine instance
 * Should be called only a after all RELP functions have been terminated.
 * Terminates librelp operations, no calls are permitted after engine 
 * destruction.
 */
relpRetVal
relpEngineDestruct(relpEngine_t **ppThis)
{
	relpEngine_t *pThis;

	ENTER_RELPFUNC;
	assert(ppThis != NULL);
	pThis = *ppThis;
	RELPOBJ_assert(pThis, Engine);

	/* TODO: check for pending operations -- rgerhards, 2008-03-16 */

	pthread_mutex_destroy(&pThis->mutSrvLst);
	pthread_mutex_destroy(&pThis->mutSessLst);
	/* done with de-init work, now free engine object itself */
	free(pThis);
	*ppThis = NULL;

finalize_it:
	LEAVE_RELPFUNC;
}


static void dbgprintDummy(char __attribute__((unused)) *fmt, ...) {}
/* set a pointer to the debug function inside the engine. To reset a debug
 * function that already has been set, provide a NULL function pointer.
 * rgerhards, 2008-03-17
 */
relpRetVal
relpEngineSetDbgprint(relpEngine_t *pThis, void (*dbgprint)(char *fmt, ...) __attribute__((format(printf, 1, 2))))
{
	ENTER_RELPFUNC;
	RELPOBJ_assert(pThis, Engine);

	pThis->dbgprint = (dbgprint == NULL) ? dbgprintDummy : dbgprint;
	LEAVE_RELPFUNC;
}


/* add a relp listener to the engine. The listen port must be provided.
 * The listen port may be NULL, in which case the default port is used.
 * rgerhards, 2008-03-17
 */
relpRetVal
relpEngineAddListner(relpEngine_t *pThis, unsigned char *pLstnPort)
{
	relpSrv_t *pSrv;
	ENTER_RELPFUNC;
	RELPOBJ_assert(pThis, Engine);

	CHKRet(relpSrvConstruct(&pSrv, pThis));
	CHKRet(relpSrvSetLstnPort(pSrv, pLstnPort));
	CHKRet(relpSrvRun(pSrv));

	/* all went well, so we can add the server to our server list */
	CHKRet(relpEngineAddToSrvList(pThis, pSrv));

finalize_it:
	LEAVE_RELPFUNC;
}


/* The "Run" method starts the relp engine. Most importantly, this means the engine begins
 * to read and write data to its peers. This method must be called on its own thread as it
 * will not return until the engine is finished. Note that the engine itself may (or may
 * not ;)) spawn additional threads. This is an implementation detail not to be card of by
 * caller.
 * Note that the engine MUST be running even if the caller intends to just SEND messages.
 * This is necessary because relp is a full-duplex protcol where acks and commands (e.g.
 * "abort" may be received at any time.
 *
 * This function is implemented as a select() server. I know that epoll() wold probably
 * be much better, but I implement the first version as select() because of portability.
 * Once everything has matured, we may begin to provide performance-optimized versions for
 * the several flavours of enhanced OS APIs.
 * rgerhards, 2008-03-17
 */
relpRetVal
relpEngineRun(relpEngine_t *pThis)
{
	relpEngSrvLst_t *pSrvEtry;
	relpEngSessLst_t *pSessEtry;
	relpEngSessLst_t *pSessEtryNext;
	relpSess_t *pNewSess;
	relpRetVal localRet;
	int iSocks;
	int sock;
	int maxfds;
	int nfds;
	int i;
	int iTCPSess;
	fd_set readfds;

	ENTER_RELPFUNC;
	RELPOBJ_assert(pThis, Engine);

	/* this is an endless loop - TODO: decide how to terminate */
	while(1) {
	        maxfds = 0;
	        FD_ZERO (&readfds);

		/* Add the listen sockets to the list of read descriptors.  */
		for(pSrvEtry = pThis->pSrvLstRoot ; pSrvEtry != NULL ; pSrvEtry = pSrvEtry->pNext) {
			for(iSocks = 1 ; iSocks <= relpSrvGetNumLstnSocks(pSrvEtry->pSrv) ; ++iSocks) {
				sock = relpSrvGetLstnSock(pSrvEtry->pSrv, iSocks);
				FD_SET(sock, &readfds);
				if(sock > maxfds) maxfds = sock;
			}
		}

		/* Add all sessions for reception (they all have just one socket) */
		for(pSessEtry = pThis->pSessLstRoot ; pSessEtry != NULL ; pSessEtry = pSessEtry->pNext) {
			sock = relpSessGetSock(pSessEtry->pSess);
			FD_SET(sock, &readfds);
			if(sock > maxfds) maxfds = sock;
		}

		/* TODO: add send sockets (if needed) */

		/* done adding all sockets */
		if(pThis->dbgprint != dbgprintDummy) {
			pThis->dbgprint("***<librelp> calling select, active file descriptors (max %d): ", maxfds);
			for(nfds = 0; nfds <= maxfds; ++nfds)
				if(FD_ISSET(nfds, &readfds))
					dbgprintf("%d ", nfds);
			pThis->dbgprint("\n");
		}

		/* wait for io to become ready */
		nfds = select(maxfds+1, (fd_set *) &readfds, NULL, NULL, NULL);
pThis->dbgprint("relp select returns, nfds %d\n", nfds);
	
		/* and then start again with the servers (new connection request) */
		for(pSrvEtry = pThis->pSrvLstRoot ; pSrvEtry != NULL ; pSrvEtry = pSrvEtry->pNext) {
			for(iSocks = 1 ; iSocks <= relpSrvGetNumLstnSocks(pSrvEtry->pSrv) ; ++iSocks) {
				sock = relpSrvGetLstnSock(pSrvEtry->pSrv, iSocks);
				if(FD_ISSET(sock, &readfds)) {
					pThis->dbgprint("new connect on RELP socket #%d\n", sock);
					localRet = relpSessAcceptAndConstruct(&pNewSess, pSrvEtry->pSrv, sock);
					if(localRet == RELP_RET_OK) {
						localRet = relpEngineAddToSess(pThis, pNewSess);
					}
					/* TODO: check localret, emit error msg! */
					--nfds; /* indicate we have processed one */
				}
			}
		}

		/* now check if we have some data waiting for sessions */
		for(pSessEtry = pThis->pSessLstRoot ; pSessEtry != NULL ; ) {
			pSessEtryNext = pSessEtry->pNext; /* we need to cache this as we may delete the entry! */
			sock = relpSessGetSock(pSessEtry->pSess);
			if(FD_ISSET(sock, &readfds)) {
				localRet = relpSessRcvData(pSessEtry->pSess); /* errors are handled there */
				/* if we had an error during processing, we must shut down the session. This
				 * is part of the protocol specification: errors are recovered by aborting the
				 * session, which may eventually be followed by a new connect.
				 */
				if(localRet != RELP_RET_OK) {
					pThis->dbgprint("relp session %d iRet %d, tearing it down\n",
						        sock, localRet);
					relpEngineDelSess(pThis, pSessEtry);
				}
				--nfds; /* indicate we have processed one */
			}
			pSessEtry = pSessEtryNext;
		}

	}

	LEAVE_RELPFUNC;
}


/* process an incoming command
 * This function receives a RELP frame and dispatches it to the correct
 * command handler. If the command is unknown, it responds with an error
 * return state but does not process anything. The frame is NOT destructed
 * by this function - this must be done by the caller.
 * rgerhards, 2008-03-17
 */
relpRetVal
relpEngineDispatchFrame(relpEngine_t *pThis, relpSess_t *pSess, relpFrame_t *pFrame)
{
	ENTER_RELPFUNC;
printf("relp oid %d\n", pThis->objID);
	RELPOBJ_assert(pThis, Engine);
	RELPOBJ_assert(pSess, Sess);
	RELPOBJ_assert(pFrame, Frame);

	pThis->dbgprint("relp engine is dispatching frame with command '%s'\n", pFrame->cmd);

	/* currently, we hardcode the commands. Over time, they may be dynamically 
	 * loaded and, when so, should come from a linked list. TODO -- rgerhards, 2008-03-17
	 */
	if(!strcmp(pFrame->cmd, "init")) {
		CHKRet(relpSCInit(pFrame, pSess));
	} else if(!strcmp(pFrame->cmd, "go")) {
		pThis->dbgprint("relp will be calling go command");
	} else {
		pThis->dbgprint("invalid or not supported relp command '%s'\n", pFrame->cmd);
		ABORT_FINALIZE(RELP_RET_INVALID_CMD);
	}

finalize_it:
	LEAVE_RELPFUNC;
}