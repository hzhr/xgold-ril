/* Infineon XGOLD RIL
**
** Copyright (C) 2008, Texas Instruments
** Copyright 2006, The Android Open Source Project
**
** Licensed under the Apache License, Version 2.0 (the "License");
** you may not use this file except in compliance with the License.
** You may obtain a copy of the License at
**
**     http://www.apache.org/licenses/LICENSE-2.0
**
** Unless required by applicable law or agreed to in writing, software
** distributed under the License is distributed on an "AS IS" BASIS,
** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
** See the License for the specific language governing permissions and
** limitations under the License.
** 
** Based on reference-ril by - Copyright 2006, The Android Open Source Project
** Modified October 2008 by Texas Instruments
*/

#include <telephony/ril.h>
#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <pthread.h>
#include <alloca.h>
#include "atchannel.h"
#include "at_tok.h"
#include "misc.h"
#include <getopt.h>
#include <sys/socket.h>
#include <cutils/sockets.h>
#include <cutils/properties.h>
#include <termios.h>

#define LOG_TAG "RIL"
#include <utils/Log.h>

#define MAX_AT_RESPONSE 0x1000

/* pathname returned from RIL_REQUEST_SETUP_DATA_CALL / RIL_REQUEST_SETUP_DEFAULT_PDP */
#define PPP_TTY_PATH "ppp0"

#ifdef USE_TI_COMMANDS

// Enable a workaround
// 1) Make incoming call, do not answer
// 2) Hangup remote end
// Expected: call should disappear from CLCC line
// Actual: Call shows as "ACTIVE" before disappearing
#define WORKAROUND_ERRONEOUS_ANSWER 1

// Some varients of the TI stack do not support the +CGEV unsolicited
// response. However, they seem to send an unsolicited +CME ERROR: 150
#define WORKAROUND_FAKE_CGEV 1
#endif

// Workaround to enable audio modem as the audio needs to be started just after the call.
// it can not be managed by the Audio HAL.
#define WORKAROUND_MODEM_AUDIO

typedef enum {
    SIM_ABSENT = 0,
    SIM_NOT_READY = 1,
    SIM_READY = 2, /* SIM_READY means the radio state is RADIO_STATE_SIM_READY */
    SIM_PIN = 3,
    SIM_PUK = 4,
    SIM_NETWORK_PERSONALIZATION = 5
} SIM_Status; 

static void onRequest (int request, void *data, size_t datalen, RIL_Token t);
static RIL_RadioState currentState();
static int onSupports (int requestCode);
static void onCancel (RIL_Token t);
static const char *getVersion();
static int isRadioOn();
static SIM_Status getSIMStatus();
static int getCardStatus(RIL_CardStatus **pp_card_status);
static void freeCardStatus(RIL_CardStatus *p_card_status);
static void onDataCallListChanged(void *param);
static void requestQueryCallWaiting(void *data, size_t datalen, RIL_Token t);
static void requestQueryCallForwardStatus(RIL_Token t);
static void requestSetCallForward(void *data, RIL_Token t);
static void requestGetCLIR(void *data, size_t datalen, RIL_Token t);
static void requestSetCLIR(void *data, size_t datalen, RIL_Token t);
static void requestSendSMSExpectMore(void *data, size_t datalen, RIL_Token t);
static void requestSetFacilityLock(void *data, size_t datalen, RIL_Token t);
static void requestChangeBarringPassword(void *data, size_t datalen, RIL_Token t);
static void requestSetNetworkSelectionManual(void *data, size_t datalen, RIL_Token t);
static void requestQueryCLIP(void *data, size_t datalen, RIL_Token t);
static void requestResetRadio(RIL_Token t);
static void requestSetSuppSVCNotification(void *data, size_t datalen, RIL_Token t);
static void requestExplicitCallTransfer(RIL_Token t);
static void requestSetLocationUpdates(void *data, size_t datalen, RIL_Token t);
static void requestSTKGetprofile(RIL_Token t);
static void requestSTKSetProfile(void * data, size_t datalen, RIL_Token t);
static void requestLastFailCause(RIL_Token t);
static void requestOEMHookStrings(void * data, size_t datalen, RIL_Token t);
static void requestSTKSendEnvelopeCommand(void * data, size_t datalen, RIL_Token t);
static void requestSTKSendTerminalResponse(void * data, size_t datalen, RIL_Token t);
static void requestSetCallWaiting(void *data, size_t datalen, RIL_Token t);
static void requestNotSupported(RIL_Token t);

extern const char * requestToString(int request);

/*** Static Variables ***/
static const RIL_RadioFunctions s_callbacks = {
    RIL_VERSION,
    onRequest,
    currentState,
    onSupports,
    onCancel,
    getVersion
};

#ifdef RIL_SHLIB
static const struct RIL_Env *s_rilenv;

#define RIL_onRequestComplete(t, e, response, responselen) s_rilenv->OnRequestComplete(t,e, response, responselen)
#define RIL_onUnsolicitedResponse(a,b,c) s_rilenv->OnUnsolicitedResponse(a,b,c)
#define RIL_requestTimedCallback(a,b,c) s_rilenv->RequestTimedCallback(a,b,c)
#endif

static RIL_RadioState sState = RADIO_STATE_UNAVAILABLE;

static pthread_mutex_t s_state_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_state_cond = PTHREAD_COND_INITIALIZER;

static pthread_mutex_t s_reset_mutex = PTHREAD_MUTEX_INITIALIZER;

static int s_port = -1;
static const char * s_device_path = NULL;
static int          s_device_socket = 0;

/* trigger change to this with s_state_cond */
static int s_closed = 0;

static int sFD;     /* file desc of AT channel */
static char sATBuffer[MAX_AT_RESPONSE+1];
static char *sATBufferCur = NULL;
static char *sNITZtime = NULL;

/*
   We are using +XCIEV: indications instead of +CSQ and storing
   values for Android to pick up.
 */
static int at_csq_rssi = 99;
static int at_csq_ber = 99;

static const struct timeval TIMEVAL_SIMPOLL = {1,0};
static const struct timeval TIMEVAL_CALLSTATEPOLL = {0,500000};
static const struct timeval TIMEVAL_0 = {0,0};

#ifdef WORKAROUND_ERRONEOUS_ANSWER
// Max number of times we'll try to repoll when we think
// we have a AT+CLCC race condition
#define REPOLL_CALLS_COUNT_MAX 4

// Line index that was incoming or waiting at last poll, or -1 for none
static int s_incomingOrWaitingLine = -1;
// Number of times we've asked for a repoll of AT+CLCC
static int s_repollCallsCount = 0;
// Should we expect a call to be answered in the next CLCC?
static int s_expectAnswer = 0;
#endif /* WORKAROUND_ERRONEOUS_ANSWER */

#ifdef WORKAROUND_MODEM_AUDIO
    static int ifxaudio = 0;
    static char activeCallState = 0;
#endif

// Fix to pick up <AcT> info from +COPS
// Temporary to be removed for C-AT firmware
//#define WORKAROUND_ACT_FROM_COPS_FIX
#ifdef WORKAROUND_ACT_FROM_COPS_FIX
static int cops_access_technology;
#endif

static void pollSIMState (void *param);
static void setRadioState(RIL_RadioState newState);

static void HexStr_to_DecInt(char *strings, unsigned int *ints)
{
  int i = 0;
  int j = strlen(strings);
  int k = 0;
  for(i = 0, k = 0; i < j; i += 2, k++)
    {
      printf("%d, %d\n", i, k);
      if(strings[i] <= 57){
	*(ints + k) += (unsigned int)((strings[i] - 48) * 16);
      }
      else{
	*(ints+k) += (unsigned int)(((strings[i] - 97) + 10) * 16);
      }
      
      if(strings[i+1] <= 57){
	*(ints+k) += (unsigned int)(strings[i+1] - 48);
      }
      else{
	*(ints+k) += (unsigned int)((strings[i+1] - 97) + 10);
      }
    }
}

static int clccStateToRILState(int state, RIL_CallState *p_state)

{
    switch(state) {
        case 0: *p_state = RIL_CALL_ACTIVE;   return 0;
        case 1: *p_state = RIL_CALL_HOLDING;  return 0;
        case 2: *p_state = RIL_CALL_DIALING;  return 0;
        case 3: *p_state = RIL_CALL_ALERTING; return 0;
        case 4: *p_state = RIL_CALL_INCOMING; return 0;
        case 5: *p_state = RIL_CALL_WAITING;  return 0;
        default: return -1;
    }
}

static const char* networkStatusToRilString(int state)
{
  switch(state){
  case 0: return("unknown");   break;
  case 1: return("available"); break;
  case 2: return("current");   break;
  case 3: return("forbidden"); break;
  default: return NULL;
  }
}



static int preferredRatToRilRat(int rat)
{
  /* Android expects:
     0 WCDMA preferred (auto mode)
     1 GSM only
     2 WCDMA only

     IFX modem provides:
     0 GSM single mode
     1 GSM / UMTS Dual mode
     2 UTRAN (UMTS)  */

  switch(rat){
  case 0: return(1); break;
  case 1: return(0); break;
  case 2: return(2); break;
  default: return 0;
  }
}

static int XGoldRSSITo3gpp(int xgold_rssi)
{
  switch(xgold_rssi){
  case 0: return(3); break;
  case 1: return(7); break;
  case 2: return(11); break;
  case 3: return(15); break;
  case 4: return(19); break;
  case 5: return(23); break;
  case 6: return(27); break;
  case 7: return(31); break;
  default: return 99;
  }
}

/**
 * Note: directly modified line and has *p_call point directly into
 * modified line
 */
static int callFromCLCCLine(char *line, RIL_Call *p_call)
{
        //+CLCC: 1,0,2,0,0,\"+18005551212\",145
        //     index,isMT,state,mode,isMpty(,number,TOA)?

    int err;
    int state;
    int mode;

    err = at_tok_start(&line);
    if (err < 0) goto error;

    err = at_tok_nextint(&line, &(p_call->index));
    if (err < 0) goto error;

    err = at_tok_nextbool(&line, &(p_call->isMT));
    if (err < 0) goto error;

    err = at_tok_nextint(&line, &state);
    if (err < 0) goto error;

    err = clccStateToRILState(state, &(p_call->state));
    if (err < 0) goto error;

    err = at_tok_nextint(&line, &mode);
    if (err < 0) goto error;

    p_call->isVoice = (mode == 0);

    err = at_tok_nextbool(&line, &(p_call->isMpty));
    if (err < 0) goto error;

    if (at_tok_hasmore(&line)) {
        err = at_tok_nextstr(&line, &(p_call->number));

        /* tolerate null here */
        if (err < 0) return 0;

        // Some lame implementations return strings
        // like "NOT AVAILABLE" in the CLCC line
        if (p_call->number != NULL
            && 0 == strspn(p_call->number, "+0123456789")
        ) {
            p_call->number = NULL;
        }

        err = at_tok_nextint(&line, &p_call->toa);
        if (err < 0) goto error;
    }

    return 0;

error:
    LOGE("invalid CLCC line\n");
    return -1;
}


/** do post-AT+CFUN=1 initialization */
static void onRadioPowerOn()
{
#ifdef USE_TI_COMMANDS
    /*  Must be after CFUN=1 */
    /*  TI specific -- notifications for CPHS things such */
    /*  as CPHS message waiting indicator */

    at_send_command("AT%CPHS=1", NULL);

    /*  TI specific -- enable NITZ unsol notifs */
    at_send_command("AT%CTZV=1", NULL);
#endif
    
    pollSIMState(NULL);
}

/** do post- SIM ready initialization */
static void onSIMReady()
{
    at_send_command_singleline("AT+CSMS=1", "+CSMS:", NULL);
    /*
     * Always send SMS messages directly to the TE
     *
     * mode = 1 // discard when link is reserved (link should never be
     *             reserved)
     * mt = 2   // most messages routed to TE
     * bm = 2   // new cell BM's routed to TE
     * ds = 1   // Status reports routed to TE
     * bfr = 1  // flush buffer
     */
    at_send_command("AT+CNMI=1,2,2,1,1", NULL);
}


#define XMM_STATE_FILE "/sys/devices/platform/xmm_mux/state"
#define XMM_STATE_VALID_FILE "/sys/devices/platform/xmm_mux/state_valid"
#define XMM_STATE_ON "ON"
#define XMM_STATE_OFF "OFF"
#define XMM_STATE_VALID_IND "STATE_VALID"
#define XMM_STATE_TRANSIENT_IND "STATE_TRANSITION"

#define XMM_STATE_POLL_INTERVAL_MS 50
//#define XMM_STATE_POLL_TIMEOUT_MS  5000
#define XMM_BUFF_SIZE 32

enum {XMM_OFF, XMM_ON};

static int xgold_power_set_state(int onoff, int timeout_ms)
{
    int status = -1, count, fd_state, fd_valid = 0, timeout;
    char *target;
    struct timespec req, rem;
    char buffer[XMM_BUFF_SIZE + 1];

    if (onoff == XMM_OFF)
        target = XMM_STATE_OFF;
    else
        target = XMM_STATE_ON;

    LOGD("XMM POWER ON/OFF: %s", target);

    fd_state = open(XMM_STATE_FILE, O_RDWR);
    if (fd_state < 0) {
        LOGE("XMM POWER ON/OFF: Can not open XMM state file");
        status = fd_state;
        goto error1;
    }
    fd_valid = open(XMM_STATE_VALID_FILE, O_RDONLY);
    if (fd_valid < 0) {
        LOGE("XMM POWER ON/OFF: Can not open XMM state valid file");
        status = fd_valid;
        goto error2;
    }

    count = write(fd_state, target, strlen(target));
    if (count < (int) strlen(target)) {
        LOGE("XMM POWER ON/OFF: Error writing ON command");
        status = count;
        goto error3;
    }

    req.tv_sec = 0;
    req.tv_nsec = XMM_STATE_POLL_INTERVAL_MS * 1000 * 1000;
    timeout = 0;
    while (timeout < timeout_ms){
        nanosleep(&req, &rem);
        timeout += XMM_STATE_POLL_INTERVAL_MS;
        lseek(fd_valid, 0, SEEK_SET); // force read update from driver
        count = read(fd_valid, buffer, XMM_BUFF_SIZE);
        if (count < 0) {
            LOGE("XMM POWER ON/OFF: Error reading modem status validity");
            status = count;
            goto error3;
        } else
            buffer[count] = '\0';
        if (strstr(buffer, XMM_STATE_TRANSIENT_IND)) {
            continue;
        } else if (strstr(buffer, XMM_STATE_VALID_IND)) {
            lseek(fd_state, 0, SEEK_SET); // force read update from driver
            count = read(fd_state, buffer, XMM_BUFF_SIZE);
            if (count < 0) {
                LOGE("XMM POWER ON/OFF: Error reading modem status");
                status = count;
                goto error3;
            } else
                buffer[count] = '\0';
            if (strstr(buffer, target)) {
                status = 0;
            }
            else {
                LOGE("XMM POWER ON/OFF: transition failed");
            }
            break;
        } else {
            LOGE("XMM POWER ON/OFF: unkwnown state validity");
            goto error3;
        }
    }

    if (timeout >= timeout_ms)
        LOGE("XMM POWER ON/OFF: TIMEOUT");

error3:
    close (fd_valid);
error2:
    close (fd_state);
error1:
    return status;
}

static int xgold_power_off(void)
{
    ATResponse *p_response = NULL;

    /* Disable any unsollicited responses
       Disregard errors - procedure will complete */
    at_send_command("AT+CGEREP=1,0", &p_response);
    at_send_command("AT+CREG=0", &p_response);
    at_send_command("AT+CGREG=0", &p_response);
    at_send_command("AT+CFUN=0", &p_response);
    at_response_free(p_response);

//    xgold_power_set_state(XMM_OFF, 2000);
    return 0; // Always succesfull
}

static void xgold_reset()
{
	LOGD("Time out! Reset modem");
	property_set("net.ppp", "hang");
}

static int xgold_power_on(void)
{
    int err;
    ATResponse *p_response = NULL;

//    err = xgold_power_set_state(XMM_ON, 10000);
//    if (err < 0)
//        goto error;
	LOGD("Set time out handler");

	at_set_on_timeout(xgold_reset);

	at_set_on_reader_closed(xgold_reset);

    at_send_command("AT+CFUN=1", NULL);

   /* Sequence derived from initializeCallback()... */
    at_handshake();

    /* note: we don't check errors here. Everything important will
       be handled in onATTimeout and onATReaderClosed */

    /*  atchannel is tolerant of echo but it must */
    /*  have verbose result codes */
    at_send_command("ATE0Q0V1", NULL);

    /*  No auto-answer */
    at_send_command("ATS0=0", NULL);

    /*  Extended errors */
    at_send_command("AT+CMEE=1", NULL);

    /*  Network registration events */
    err = at_send_command("AT+CREG=2", &p_response);

    /* some handsets -- in tethered mode -- don't support CREG=2 */
    if (err < 0 || p_response->success == 0) {
        at_send_command("AT+CREG=1", NULL);
    }

    at_response_free(p_response);

    /*  GPRS registration events */
    at_send_command("AT+CGREG=2", NULL);

    /*  Call Waiting notifications */
    at_send_command("AT+CCWA=1", NULL);

    /*  Alternating voice/data off */
    at_send_command("AT+CMOD=0", NULL);

    /*  Not muted */
//    at_send_command("AT+CMUT=0", NULL);

    /*  +CSSU unsolicited supp service notifications */
    at_send_command("AT+CSSN=0,1", NULL);

  /*  No connected line identification */
  at_send_command("AT+COLP=0", NULL);

  /*  USSD unsolicited */
  at_send_command("AT+CUSD=1", NULL);

    /*  HEX character set */
//    at_send_command("AT+CSCS=\"HEX\"", NULL);

  /*  Enable +CGEV GPRS event notifications, but don't buffer */
  at_send_command("AT+CGEREP=2,1", NULL);

  /*  SMS PDU mode */
  at_send_command("AT+CMGF=0", NULL);

  /* Enable NITZ reporting */
  at_send_command("AT+CTZU=1", NULL);
  at_send_command("AT+CTZR=1", NULL);

  /* Enable unsolizited RSSI reporting */
  at_send_command("AT+XMER=1", NULL);

  /* Preferred RAT - UMTS Dualmode */
  at_send_command("AT+XRAT=1,2", NULL);

error:
    return err;
}

static void requestRadioPower(void *data, size_t datalen, RIL_Token t)
{
    int onOff;

    int err;
    ATResponse *p_response = NULL;

    assert (datalen >= sizeof(int *));
    onOff = ((int *)data)[0];

    if (onOff == 0 && sState != RADIO_STATE_OFF) {
#if 0
        err = at_send_command("AT+CFUN=0", &p_response);
        if (err < 0 || p_response->success == 0) goto error;
#endif
        err = xgold_power_off();
        if (err < 0) goto error;
        setRadioState(RADIO_STATE_OFF);
    } else if (onOff > 0 && sState == RADIO_STATE_OFF) {
        err = xgold_power_on();
        if (err < 0)
            goto error;
#if 0
        err = at_send_command("AT+CFUN=1", &p_response);
        if (err < 0|| p_response->success == 0) {
            // Some stacks return an error when there is no SIM,
            // but they really turn the RF portion on
            // So, if we get an error, let's check to see if it
            // turned on anyway

            if (isRadioOn() != 1) {
                goto error;
            }
        }
#endif
        setRadioState(RADIO_STATE_SIM_NOT_READY);
    }

    at_response_free(p_response);
    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
    return;

error:
    at_response_free(p_response);
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
}

static void requestOrSendDataCallList(RIL_Token *t);

static void onDataCallListChanged(void *param)
{
    requestOrSendDataCallList(NULL);
}

static void requestDataCallList(void *data, size_t datalen, RIL_Token t)
{
    requestOrSendDataCallList(&t);
}

static void requestOrSendDataCallList(RIL_Token *t)
{
    ATResponse *p_response;
    ATLine *p_cur;
    int err;
    int n = 0;
    char *out;

    err = at_send_command_multiline ("AT+CGACT?", "+CGACT:", &p_response);
    if (err != 0 || p_response->success == 0) {
        if (t != NULL)
            RIL_onRequestComplete(*t, RIL_E_GENERIC_FAILURE, NULL, 0);
        else
            RIL_onUnsolicitedResponse(RIL_UNSOL_DATA_CALL_LIST_CHANGED,
                                      NULL, 0);
        return;
    }

    for (p_cur = p_response->p_intermediates; p_cur != NULL;
         p_cur = p_cur->p_next)
        n++;

    RIL_Data_Call_Response *responses =
        alloca(n * sizeof(RIL_Data_Call_Response));

    int i;
    for (i = 0; i < n; i++) {
        responses[i].cid = -1;
        responses[i].active = -1;
        responses[i].type = "";
        responses[i].apn = "";
        responses[i].address = "";
    }

    RIL_Data_Call_Response *response = responses;
    for (p_cur = p_response->p_intermediates; p_cur != NULL;
         p_cur = p_cur->p_next) {
        char *line = p_cur->line;

        err = at_tok_start(&line);
        if (err < 0)
            goto error;

        err = at_tok_nextint(&line, &response->cid);
        if (err < 0)
            goto error;

        err = at_tok_nextint(&line, &response->active);
        if (err < 0)
            goto error;

        response++;
    }

    at_response_free(p_response);

    err = at_send_command_multiline ("AT+CGDCONT?", "+CGDCONT:", &p_response);
    if (err != 0 || p_response->success == 0) {
        if (t != NULL)
            RIL_onRequestComplete(*t, RIL_E_GENERIC_FAILURE, NULL, 0);
        else
            RIL_onUnsolicitedResponse(RIL_UNSOL_DATA_CALL_LIST_CHANGED,
                                      NULL, 0);
        return;
    }

    for (p_cur = p_response->p_intermediates; p_cur != NULL;
         p_cur = p_cur->p_next) {
        char *line = p_cur->line;
        int cid;
        char *type;
        char *apn;
        char *address;


        err = at_tok_start(&line);
        if (err < 0)
            goto error;

        err = at_tok_nextint(&line, &cid);
        if (err < 0)
            goto error;

        for (i = 0; i < n; i++) {
            if (responses[i].cid == cid)
                break;
        }

        if (i >= n) {
            /* details for a context we didn't hear about in the last request */
            continue;
        }

        err = at_tok_nextstr(&line, &out);
        if (err < 0)
            goto error;

        responses[i].type = alloca(strlen(out) + 1);
        strcpy(responses[i].type, out);

        err = at_tok_nextstr(&line, &out);
        if (err < 0)
            goto error;

        responses[i].apn = alloca(strlen(out) + 1);
        strcpy(responses[i].apn, out);

        err = at_tok_nextstr(&line, &out);
        if (err < 0)
            goto error;

        responses[i].address = alloca(strlen(out) + 1);
        strcpy(responses[i].address, out);
    }

    at_response_free(p_response);

    if (t != NULL)
        RIL_onRequestComplete(*t, RIL_E_SUCCESS, responses,
                              n * sizeof(RIL_Data_Call_Response));
    else
        RIL_onUnsolicitedResponse(RIL_UNSOL_DATA_CALL_LIST_CHANGED,
                                  responses,
                                  n * sizeof(RIL_Data_Call_Response));

    return;

error:
    if (t != NULL)
        RIL_onRequestComplete(*t, RIL_E_GENERIC_FAILURE, NULL, 0);
    else
        RIL_onUnsolicitedResponse(RIL_UNSOL_DATA_CALL_LIST_CHANGED,
                                  NULL, 0);

    at_response_free(p_response);
}

static void requestBasebandVersion(void *data, size_t datalen, RIL_Token t)
{
    int err;
    ATResponse *p_response = NULL;
    char * response = NULL;
    char* line = NULL;

    err = at_send_command_singleline("AT+CGMM", "", &p_response);
    if (err != 0) goto error;

    line = p_response->p_intermediates->line;

    response = (char *)alloca(sizeof(char *));

    err = at_tok_nextstr(&line, &response);    
    if (err < 0) goto error;

    RIL_onRequestComplete(t, RIL_E_SUCCESS, response, sizeof(char *));
    at_response_free(p_response);
    return;

error:
    at_response_free(p_response);
    LOGE("ERROR: requestBasebandVersion failed\n");
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
}


static void requestQueryNetworkSelectionMode(
                void *data, size_t datalen, RIL_Token t)
{
    int err;
    ATResponse *p_response = NULL;
    int response = 0;
    char *line;

    err = at_send_command_singleline("AT+COPS?", "+COPS:", &p_response);

    if (err < 0 || p_response->success == 0) {
        goto error;
    }

    line = p_response->p_intermediates->line;

    err = at_tok_start(&line);

    if (err < 0) {
        goto error;
    }

    err = at_tok_nextint(&line, &response);

    if (err < 0) {
        goto error;
    }

    RIL_onRequestComplete(t, RIL_E_SUCCESS, &response, sizeof(int));
    at_response_free(p_response);
    return;

error:
    at_response_free(p_response);
    LOGE("requestQueryNetworkSelectionMode must never return error when radio is on");
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
}


static void requestQueryAvailableNetworks(void *data, size_t datalen, RIL_Token t)
{
  /* We expect an answer on the following form:
    +COPS: (2,"AT&T","AT&T","310410",0),(1,"T-Mobile ","TMO","310260",0) 
  */
  
    int err, operators, i, skip, status;
    ATResponse *p_response = NULL;
    char * c_skip, *line, *p = NULL;
    char ** response = NULL;

    err = at_send_command_singleline("AT+COPS=?", "+COPS:", &p_response);

    if (err != 0) goto error;

    line = p_response->p_intermediates->line;

    err = at_tok_start(&line);
    if (err < 0) goto error;

    /* Count number of '(' in the +COPS response to get number of operators*/
    operators = 0;
    for (p = line ; *p != '\0' ;p++) {
        if (*p == '(') operators++;
    }

    response = (char **)alloca(operators * 4 * sizeof(char *));

    for (i = 0 ; i < operators ; i++ ) 
      {
	err = at_tok_nextstr(&line, &c_skip);
        if (err < 0) goto error;
	status = atoi(&c_skip[1]);
	response[i*4+3] = (char*)networkStatusToRilString(status);

        err = at_tok_nextstr(&line, &(response[i*4+0]));
        if (err < 0) goto error;

        err = at_tok_nextstr(&line, &(response[i*4+1]));
        if (err < 0) goto error;

        err = at_tok_nextstr(&line, &(response[i*4+2]));
        if (err < 0) goto error;

#if 0
        err = at_tok_nextstr(&line, &c_skip);

        if (err < 0) goto error;
#endif
      }

    RIL_onRequestComplete(t, RIL_E_SUCCESS, response, (operators * 4 * sizeof(char *)));
    at_response_free(p_response);
    return;

error:
    at_response_free(p_response);
    LOGE("ERROR - requestQueryAvailableNetworks() failed");
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
}

static void requestGetPreferredNetworkType(void *data, size_t datalen, RIL_Token t)
{
    int err;
    ATResponse *p_response = NULL;
    int response = 0;
    char *line;

#if 0
    err = at_send_command_singleline("AT+XRAT?", "+XRAT:", &p_response);

    if (err < 0 || p_response->success == 0) {
        goto error;
    }

    line = p_response->p_intermediates->line;

    err = at_tok_start(&line);

    if (err < 0) {
        goto error;
    }

    err = at_tok_nextint(&line, &response);

    response = preferredRatToRilRat(response);

    if (err < 0) {
        goto error;
    }
#else
    response = 1; /* GSM */
#endif
    RIL_onRequestComplete(t, RIL_E_SUCCESS, &response, sizeof(int));
    at_response_free(p_response);
    return;

error:
    at_response_free(p_response);
    LOGE("ERROR: requestGetPreferredNetworkType() failed\n");
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);

}

static void requestSetPreferredNetworkType(void *data, size_t datalen, RIL_Token t)
{
    int err, rat;
    ATResponse *p_response = NULL;
    char * cmd = NULL;
    const char *at_rat = NULL;

    assert (datalen >= sizeof(int *));
    rat = ((int *)data)[0];

#if 0
    switch (rat)
    {
        case 0: at_rat = "1,2"; break;/* Dual Mode - WCDMA preferred*/
        case 1: at_rat = "0"; break;  /* GSM only */
        case 2: at_rat = "2"; break;  /* WCDMA only */
    }

    /* Need to unregister from NW before changing preferred RAT */
    err = at_send_command("AT+COPS=2", NULL);
    if (err < 0) goto error;

    asprintf(&cmd, "AT+XRAT=%s", at_rat);
    err = at_send_command(cmd, &p_response);
    free(cmd);

    if (err < 0|| p_response->success == 0) {
      goto error;
    }
#else
    if (rat != 1) /* not GSM */
        goto error;
#endif
    /* Register on the NW again */
    err = at_send_command("AT+COPS=0", NULL);
    if (err < 0) goto error;

    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, sizeof(int));
    at_response_free(p_response);
    return;

error:
    at_response_free(p_response);
    LOGE("ERROR: requestSetPreferredNetworkType() failed\n");
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
}

static void requestNeighboringCellIds(void *data, size_t datalen, RIL_Token t)
{
/*
* Maximum number of neighborhood cells
* 15 is set based on AT specification. It can maximum handle 16 and that
* includes the current cell, meaning you can have 15 neighbor cells.
*/
#define MAX_NUM_NEIGHBOR_CELLS 15

    int err = 0;
    char *p = NULL;
    int n = 0;
    ATLine *tmp = NULL;
    ATResponse *atresponse = NULL;
    RIL_NeighboringCell *ptr_cells[MAX_NUM_NEIGHBOR_CELLS];

    /* err = at_send_command_multiline("AT+CGED=1", "+CGED:", &atresponse); */ /* AT+CGED=1 == auto report? */
    err = at_send_command_multiline("AT+CGED=0", "+CGED:", &atresponse);
    if (err < 0 ||
        atresponse->success == 0 || atresponse->p_intermediates == NULL)
        goto error;

    tmp = atresponse->p_intermediates;
    while (tmp) {
        if (n > MAX_NUM_NEIGHBOR_CELLS)
            goto error;
        p = tmp->line;
        if (strstr(p, "Neighbour Cell ")) {
            char *line = NULL;
            char *skip = NULL;
            int mcc = 0;
            int mnc = 0;
            int lac = 0;
            int ci = 0;
            int bsic = 0;
            int arfcn = 0;
            int rxlev = 0;

            tmp = tmp->p_next;
            line = tmp->line;

/* format:
Neighbour Cell 1:
MCC:xxx, MNC:  0, LAC:xxxx, CI:xxxx, BSIC:xx Arfcn:000xx, RxLev:0xx, 
C1_nc:000xx, C2_nc:000xx,
*/
            /* MCC */
            err = at_tok_start(&line);
            if (err < 0)
                goto error;
            err = at_tok_nexthexint(&line, &mcc);
            if (err < 0)
                goto error;

            /* MNC */
            err = at_tok_start(&line);
            if (err < 0)
                goto error;
            err = at_tok_nexthexint(&line, &mnc);
            if (err < 0)
                goto error;

            /* LAC */
            err = at_tok_start(&line);
            if (err < 0)
                goto error;
            err = at_tok_nexthexint(&line, &lac);
            if (err < 0)
                goto error;
            if (lac == 0)
                goto next;

            /* CI */
            err = at_tok_start(&line);
            if (err < 0)
                goto error;
            err = at_tok_nexthexint(&line, &ci);
            if (err < 0)
                goto error;
            if (ci == 0xffff)
                goto next;

            /* BSIC */
            err = at_tok_start(&line);
            if (err < 0)
                goto error;
            err = at_tok_nexthexint(&line, &bsic);
            if (err < 0)
                goto error;

            /* Arfcn */
            err = at_tok_start(&line);
            if (err < 0)
                goto error;
            err = at_tok_nextint(&line, &arfcn);
            if (err < 0)
                goto error;

            /* RxLev */
            err = at_tok_start(&line);
            if (err < 0)
                goto error;
            err = at_tok_nextint(&line, &rxlev);
            if (err < 0)
                goto error;

            /* process data for each cell */
            ptr_cells[n] = alloca(sizeof(RIL_NeighboringCell));
            ptr_cells[n]->rssi = rxlev;
            ptr_cells[n]->cid = alloca(9 * sizeof(char));
            sprintf(ptr_cells[n]->cid, "%04x%04x", lac, ci);
            n++;
        }
next:
        tmp = tmp->p_next;
    }

    RIL_onRequestComplete(t, RIL_E_SUCCESS, ptr_cells,
                          n * sizeof(RIL_NeighboringCell *));

finally:
    at_response_free(atresponse);
    return;

error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    goto finally;
}

static void requestQueryFacilityLock(void *data, size_t datalen, RIL_Token t)
{
    int err, rat, response;
    ATResponse *p_response = NULL;
    char * cmd = NULL;
    char * line = NULL;
    char * facility_string = NULL;
    char * facility_password = NULL;
    char * facility_class = NULL;

    LOGD("FACILITY");
    assert (datalen >=  (3 * sizeof(char **)));

    facility_string   = ((char **)data)[0];
    facility_password = ((char **)data)[1];
    facility_class    = ((char **)data)[2];
  

    asprintf(&cmd, "AT+CLCK=\"%s\",2,\"%s\",%s", facility_string, facility_password, facility_class);
    err = at_send_command_singleline(cmd,"+CLCK:", &p_response);
    free(cmd);
    if (err < 0 || p_response->success == 0){
      goto error;
    }

    line = p_response->p_intermediates->line;

    err = at_tok_start(&line);

    if (err < 0) {
        goto error;
    }

    err = at_tok_nextint(&line, &response);

    if (err < 0) {
        goto error;
    }

    RIL_onRequestComplete(t, RIL_E_SUCCESS, &response, sizeof(int));
    at_response_free(p_response);
    return;

error:
    at_response_free(p_response);
    LOGE("ERROR: requestQueryFacilityLock() failed\n");
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);  
}
					  
static void sendCallStateChanged(void *param)
{
    RIL_onUnsolicitedResponse (
        RIL_UNSOL_RESPONSE_CALL_STATE_CHANGED,
        NULL, 0);
}

static void requestGetCurrentCalls(void *data, size_t datalen, RIL_Token t)
{
    int err;
    ATResponse *p_response;
    ATLine *p_cur;
    int countCalls;
    int countValidCalls;
    RIL_Call *p_calls;
    RIL_Call **pp_calls;
    int i;
    int needRepoll = 0;
#ifdef WORKAROUND_MODEM_AUDIO
    char activeCallFound;
#endif

#ifdef WORKAROUND_ERRONEOUS_ANSWER
    int prevIncomingOrWaitingLine;

    prevIncomingOrWaitingLine = s_incomingOrWaitingLine;
    s_incomingOrWaitingLine = -1;
#endif /*WORKAROUND_ERRONEOUS_ANSWER*/

    if(currentState() != RADIO_STATE_SIM_READY){
      /* Might be waiting for SIM PIN */
      RIL_onRequestComplete(t, RIL_E_RADIO_NOT_AVAILABLE, NULL, 0);
    }

    err = at_send_command_multiline ("AT+CLCC", "+CLCC:", &p_response);

    if (err != 0 || p_response->success == 0) {
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
        return;
    }

    /* count the calls */
    for (countCalls = 0, p_cur = p_response->p_intermediates
            ; p_cur != NULL
            ; p_cur = p_cur->p_next
    ) {
        countCalls++;
    }

    /* yes, there's an array of pointers and then an array of structures */

    pp_calls = (RIL_Call **)alloca(countCalls * sizeof(RIL_Call *));
    p_calls = (RIL_Call *)alloca(countCalls * sizeof(RIL_Call));
    memset (p_calls, 0, countCalls * sizeof(RIL_Call));

    /* init the pointer array */
    for(i = 0; i < countCalls ; i++) {
        pp_calls[i] = &(p_calls[i]);
    }

    for (countValidCalls = 0, p_cur = p_response->p_intermediates
            ; p_cur != NULL
            ; p_cur = p_cur->p_next
    ) {
        err = callFromCLCCLine(p_cur->line, p_calls + countValidCalls);

        if (err != 0) {
            continue;
        }

#ifdef WORKAROUND_ERRONEOUS_ANSWER
        if (p_calls[countValidCalls].state == RIL_CALL_INCOMING
            || p_calls[countValidCalls].state == RIL_CALL_WAITING
        ) {
            s_incomingOrWaitingLine = p_calls[countValidCalls].index;
        }
#endif /*WORKAROUND_ERRONEOUS_ANSWER*/

        if (p_calls[countValidCalls].state != RIL_CALL_ACTIVE
            && p_calls[countValidCalls].state != RIL_CALL_HOLDING
        ) {
            needRepoll = 1;
        }

        countValidCalls++;
    }

#ifdef WORKAROUND_ERRONEOUS_ANSWER
    // Basically:
    // A call was incoming or waiting
    // Now it's marked as active
    // But we never answered it
    //
    // This is probably a bug, and the call will probably
    // disappear from the call list in the next poll
    if (prevIncomingOrWaitingLine >= 0
            && s_incomingOrWaitingLine < 0
            && s_expectAnswer == 0
    ) {
        for (i = 0; i < countValidCalls ; i++) {

            if (p_calls[i].index == prevIncomingOrWaitingLine
                    && p_calls[i].state == RIL_CALL_ACTIVE
                    && s_repollCallsCount < REPOLL_CALLS_COUNT_MAX
            ) {
                LOGI(
                    "Hit WORKAROUND_ERRONOUS_ANSWER case."
                    " Repoll count: %d\n", s_repollCallsCount);
                s_repollCallsCount++;
                goto error;
            }
        }
    }

    s_expectAnswer = 0;
    s_repollCallsCount = 0;
#endif /*WORKAROUND_ERRONEOUS_ANSWER*/


// Enable audio mode is a call is active otherwise stop audio
#ifdef WORKAROUND_MODEM_AUDIO
if (ifxaudio == 1) {
    #define	AUDIOSTATE(state) (state ? "ON":"OFF")
        for (i = 0; i < countValidCalls ; i++) {
            if (p_calls[i].state == RIL_CALL_ACTIVE) {
                activeCallFound = 1;
                goto next;
            } else {
                activeCallFound = 0;
            }
    }

    next:
        if (activeCallFound != activeCallState) {
            LOGV("Audio state change from %s to %s",
                AUDIOSTATE(activeCallState), AUDIOSTATE(activeCallFound));

            activeCallState = activeCallFound;

            if (activeCallState) {
                // Enable Audio
                err = at_send_command("AT+XDRV=40,4,0,0", NULL);
                if (err < 0) goto error;
                err = at_send_command("AT+XDRV=40,4,3,0,0,1,0,0,0,0,0,0,1", NULL);
                if (err < 0) goto error;
                err = at_send_command("AT+XDRV=40,5,0,0", NULL);
                if (err < 0) goto error;
                err = at_send_command("AT+XDRV=40,5,2,0,0,1,0,0,0,0,0,0,1", NULL);
                if (err < 0) goto error;
                err = at_send_command("AT+XDRV=40,6,0,2", NULL);
                if (err < 0) goto error;
                err = at_send_command("AT+XDRV=40,6,3,0", NULL);
                if (err < 0) goto error;
                err = at_send_command("AT+XDRV=40,7,0,90", NULL);
                if (err < 0) goto error;
                err = at_send_command("AT+XDRV=40,7,3,90", NULL);
                if (err < 0) goto error;
                err = at_send_command("AT+XDRV=40,8,0,90", NULL);
                if (err < 0) goto error;
                err = at_send_command("AT+XDRV=40,8,2,90", NULL);
                if (err < 0) goto error;
                err = at_send_command("AT+XDRV=40,2,0", NULL);
                if (err < 0) goto error;
                err = at_send_command("AT+XDRV=40,2,3", NULL);
                if (err < 0) goto error;
            } else {
                // Disable Audio
                err = at_send_command("AT+XDRV=40,3,0", NULL);
                if (err < 0) goto error;
                err = at_send_command("AT+XDRV=40,3,3", NULL);
                if (err < 0) goto error;
            }
        }
    } // ifxaudio
#endif


    RIL_onRequestComplete(t, RIL_E_SUCCESS, pp_calls,
            countValidCalls * sizeof (RIL_Call *));

    at_response_free(p_response);

#ifdef POLL_CALL_STATE
    if (countValidCalls) {  // We don't seem to get a "NO CARRIER" message from
                            // smd, so we're forced to poll until the call ends.
#else
    if (needRepoll) {
#endif
        RIL_requestTimedCallback (sendCallStateChanged, NULL, &TIMEVAL_CALLSTATEPOLL);
    }
    return;

error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    at_response_free(p_response);
}

static void requestDial(void *data, size_t datalen, RIL_Token t)
{
    RIL_Dial *p_dial;
    char *cmd;
    const char *clir;
    int ret;

    p_dial = (RIL_Dial *)data;

    switch (p_dial->clir) {
        case 1: clir = "I"; break;  /*invocation*/
        case 2: clir = "i"; break;  /*suppression*/
        default:
        case 0: clir = ""; break;   /*subscription default*/
    }

    asprintf(&cmd, "ATD%s%s;", p_dial->address, clir);

    ret = at_send_command(cmd, NULL);

    free(cmd);

    /* success or failure is ignored by the upper layer here.
       it will call GET_CURRENT_CALLS and determine success that way */
    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
}

static void requestWriteSmsToSim(void *data, size_t datalen, RIL_Token t)
{
    RIL_SMS_WriteArgs *p_args;
    char *cmd;
    int length;
    int err;
    ATResponse *p_response = NULL;

    p_args = (RIL_SMS_WriteArgs *)data;

    length = strlen(p_args->pdu)/2;
    asprintf(&cmd, "AT+CMGW=%d,%d", length, p_args->status);

    err = at_send_command_sms(cmd, p_args->pdu, "+CMGW:", &p_response);
    free(cmd);

    if (err != 0 || p_response->success == 0) goto error;

    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
    at_response_free(p_response);
    return;

error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    at_response_free(p_response);
}

static void requestHangup(void *data, size_t datalen, RIL_Token t)
{
    int *p_line;

    int ret;
    char *cmd;

    p_line = (int *)data;

    // 3GPP 22.030 6.5.5
    // "Releases a specific active call X"
    asprintf(&cmd, "AT+CHLD=1%d", p_line[0]);

    ret = at_send_command(cmd, NULL);

    free(cmd);

    /* success or failure is ignored by the upper layer here.
       it will call GET_CURRENT_CALLS and determine success that way */
    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
}

static void requestSignalStrength(void *data, size_t datalen, RIL_Token t)
{
    ATResponse *p_response = NULL;
    int err;
    int response[2];
    char *line;

    LOGD("Sending stored CSQ values to RIL");
    response[0] = XGoldRSSITo3gpp(at_csq_rssi);
    response[1] = at_csq_ber;
    RIL_onRequestComplete(t, RIL_E_SUCCESS, response, sizeof(response));
    return;
}

static void requestDtmfStart(void *data, size_t datalen, RIL_Token t)
{
  int err;
  char *cmd;
  char c;

  assert (datalen >= sizeof(char *));

  c = ((char *)data)[0];

  asprintf(&cmd, "AT+VTS=%c", (int)c);
  
  err = at_send_command(cmd, NULL);
  free(cmd);

  if (err != 0) goto error;

  RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
  return;

error:
    LOGE("ERROR: requestDtmfStart failed");
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
  
}

static void requestDtmfStop(void *data, size_t datalen, RIL_Token t)
{
  int err;

  /* Send a command to cancel the DTMF tone*/
  err = at_send_command("AT", NULL);
  if (err != 0) goto error;

  RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
  return;

error:
    LOGE("ERROR: requestDtmfStop failed");
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
  
}

static void requestSetMute(void *data, size_t datalen, RIL_Token t)
{
  int err;
  char *cmd;

  assert (datalen >= sizeof(int *));

  asprintf(&cmd, "AT+CMUT=%d", ((int*)data)[0]);
   
  err = at_send_command(cmd, NULL);
  free(cmd);

  if (err != 0) goto error;

  RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
  return;

error:
    LOGE("ERROR: requestSetMute failed");
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
  
}

static void requestGetMute(void *data, size_t datalen, RIL_Token t)
{
  int err, response;
  ATResponse *p_response = NULL;
  char *line;

  err = at_send_command_singleline("AT+CMUT?", "+CMUT:", &p_response);
  if (err != 0) goto error;
  
  line = p_response->p_intermediates->line;
  
  err = at_tok_start(&line);
  if (err < 0) goto error;

  err = at_tok_nextint(&line, &response);
  if (err < 0) goto error;  

  RIL_onRequestComplete(t, RIL_E_SUCCESS, &response, sizeof(int));
  return;

error:
    LOGE("ERROR: requestGetMute failed");
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);

}

static void requestScreenState(void *data, size_t datalen, RIL_Token t)
{
  int err, screenState;
    
  assert (datalen >= sizeof(int *));
  screenState = ((int*)data)[0];

  if(screenState == 1)
    {
      /* Screen is on - be sure to enable all unsolicited notifications again */
      err = at_send_command("AT+CREG=2", NULL);      
      if (err < 0) goto error;
      err = at_send_command("AT+CGREG=2", NULL);      
      if (err < 0) goto error;
      err = at_send_command("AT+CGEREP=2,1", NULL);
      if (err < 0) goto error;
	  err = at_send_command("AT+XMER=1", NULL);
	  if (err < 0) goto error;
    }
  else if(screenState == 0)
    {
      /* Screen is off - disable all unsolicited notifications */
      err = at_send_command("AT+CREG=0", NULL);      
      if (err < 0) goto error;
      err = at_send_command("AT+CGREG=0", NULL);      
      if (err < 0) goto error;
      err = at_send_command("AT+CGEREP=1,0", NULL);
      if (err < 0) goto error;
	  err = at_send_command("AT+XMER=0", NULL);
	  if (err < 0) goto error;
    }
  else
    {
      /* Not a defined value - error */
      goto error;
    }
  
  RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
  return;
  
error:
    LOGE("ERROR: requestScreenState failed");
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
}

static void requestRegistrationState(int request, void *data,
                                        size_t datalen, RIL_Token t)
{
    int err;
    int response[4];
    char * responseStr[4];
    ATResponse *p_response = NULL;
    const char *cmd;
    const char *prefix;
    char *line, *p;
    int commas;
    int skip;
    int count = 3;


    if (request == RIL_REQUEST_REGISTRATION_STATE) {
        cmd = "AT+CREG?";
        prefix = "+CREG:";
    } else if (request == RIL_REQUEST_GPRS_REGISTRATION_STATE) {
        cmd = "AT+CGREG?";
        prefix = "+CGREG:";
    } else {
        assert(0);
        goto error;
    }

    err = at_send_command_singleline(cmd, prefix, &p_response);

    if (err != 0) goto error;

    line = p_response->p_intermediates->line;

    err = at_tok_start(&line);
    if (err < 0) goto error;

    /* Ok you have to be careful here
     * The solicited version of the CREG response is
     * +CREG: n, stat, [lac, cid]
     * and the unsolicited version is
     * +CREG: stat, [lac, cid]
     * The <n> parameter is basically "is unsolicited creg on?"
     * which it should always be
     *
     * Now we should normally get the solicited version here,
     * but the unsolicited version could have snuck in
     * so we have to handle both
     *
     * Also since the LAC and CID are only reported when registered,
     * we can have 1, 2, 3, or 4 arguments here
     *
     * finally, a +CGREG: answer may have a fifth value that corresponds
     * to the network type, as in;
     *
     *   +CGREG: n, stat [,lac, cid [,networkType]]
     */

    /* count number of commas */
    commas = 0;
    for (p = line ; *p != '\0' ;p++) {
        if (*p == ',') commas++;
    }

    switch (commas) {
        case 0: /* +CREG: <stat> */
            err = at_tok_nextint(&line, &response[0]);
            if (err < 0) goto error;
            response[1] = -1;
            response[2] = -1;
        break;

        case 1: /* +CREG: <n>, <stat> */
            err = at_tok_nextint(&line, &skip);
            if (err < 0) goto error;
            err = at_tok_nextint(&line, &response[0]);
            if (err < 0) goto error;
            response[1] = -1;
            response[2] = -1;
            if (err < 0) goto error;
        break;

        case 2: /* +CREG: <stat>, <lac>, <cid> */
            err = at_tok_nextint(&line, &response[0]);
            if (err < 0) goto error;
            err = at_tok_nexthexint(&line, &response[1]);
            if (err < 0) goto error;
            err = at_tok_nexthexint(&line, &response[2]);
            if (err < 0) goto error;
        break;
        case 3: /* +CREG: <n>, <stat>, <lac>, <cid> */
            err = at_tok_nextint(&line, &skip);
            if (err < 0) goto error;
            err = at_tok_nextint(&line, &response[0]);
            if (err < 0) goto error;
            err = at_tok_nexthexint(&line, &response[1]);
            if (err < 0) goto error;
            err = at_tok_nexthexint(&line, &response[2]);
            if (err < 0) goto error;
        break;
        /* special case for CGREG, there is a fourth parameter
         * that is the network type (unknown/gprs/edge/umts)
         */
        case 4: /* +CGREG: <n>, <stat>, <lac>, <cid>, <networkType> */
            err = at_tok_nextint(&line, &skip);
            if (err < 0) goto error;
            err = at_tok_nextint(&line, &response[0]);
            if (err < 0) goto error;
            err = at_tok_nexthexint(&line, &response[1]);
            if (err < 0) goto error;
            err = at_tok_nexthexint(&line, &response[2]);
            if (err < 0) goto error;
            err = at_tok_nexthexint(&line, &response[3]);
            if (err < 0) goto error;
            count = 4;
        break;
        default:
            goto error;
    }

    asprintf(&responseStr[0], "%d", response[0]);
    asprintf(&responseStr[1], "%x", response[1]);
    asprintf(&responseStr[2], "%x", response[2]);

#ifdef WORKAROUND_ACT_FROM_COPS_FIX
    if (((response[0] == 1) || (response[0] == 5)) // registered HPLMN/RPLMN
        && (count <= 3)) { // but no <AcT> provided
        response[3] = /*cops_access_technology*/3;
        count = 4;
    }
#endif

    /* Convert 3GPP <AcT> (TS27.010 10.1.20) to ril.h interface */
    if (count > 3) {
        const int ACT_3GPP_TO_ANDROID[] = {
            1,  // GSM
            0,  // GSM Compact
            3,  // UTRAN
            2,  // EGPRS
            9,  // HSDPA
            10, // HSUPA
            11, // HSPA
            0   // E-UTRAN
        };
        if (response[3] <= 7)
            response[3] = ACT_3GPP_TO_ANDROID[response[3]];
        else
            response[3] = 0; // unknown
    }
    else  if (request == RIL_REQUEST_GPRS_REGISTRATION_STATE) {
        /* GPRS/EDGE network type workaround */
        ATResponse *p_response2 = NULL;
        cmd = "AT+XREG?";
        prefix = "+XREG:";

        LOGI("Current network registration status +XREG:");
        err = at_send_command_singleline(cmd, prefix, &p_response2);
        if (err < 0) goto out;
        line = p_response2->p_intermediates->line;
        err = at_tok_start(&line);
        if (err < 0) goto out;
        err = at_tok_nextint(&line, &skip);
        if (err < 0) goto out;
        err = at_tok_nextint(&line, &response[3]);
        if (err < 0) goto out;

        count = 4;
out:
        at_response_free(p_response2);
    }
    if (count > 3)
        asprintf(&responseStr[3], "%d", response[3]);

    RIL_onRequestComplete(t, RIL_E_SUCCESS, responseStr, count*sizeof(char*));
    at_response_free(p_response);
    return;

error:
    LOGE("requestRegistrationState must never return an error when radio is on");
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    at_response_free(p_response);
}

static void requestOperator(void *data, size_t datalen, RIL_Token t)
{
    int err;
    int i;
    int skip;
    ATLine *p_cur;
    char *response[3];
    char * cmd;
    char * line;

    memset(response, 0, sizeof(response));

    ATResponse *p_response = NULL;

    for (i = 0; i < 3; i++ ) {
      
      asprintf(&cmd, "AT+COPS=3,%d;+COPS?", i);
      err = at_send_command_singleline(cmd,"+COPS:",&p_response);
      free(cmd);
      if (err != 0 || p_response->success == 0) goto error;

      line = p_response->p_intermediates->line;

      err = at_tok_start(&line);
      if (err < 0) goto error;

      err = at_tok_nextint(&line, &skip);
      if (err < 0) goto error;

      // If we're unregistered, we may just get
      // a "+COPS: 0" response
      if (!at_tok_hasmore(&line)) {
	response[i] = NULL;
	continue;
      }
      
      err = at_tok_nextint(&line, &skip);
      if (err < 0) goto error;
      
        // a "+COPS: 0, n" response is also possible
      if (!at_tok_hasmore(&line)) {
            response[i] = NULL;
            continue;
      }
      
      err = at_tok_nextstr(&line, &(response[i]));
      if (err < 0) goto error;

#ifdef WORKAROUND_ACT_FROM_COPS_FIX
        /* Store the access technology for later use */
        err = at_tok_nextint(&line, &cops_access_technology);
        if (err < 0) goto error;
#endif
    }

    if (i != 3) {
        /* expect 3 lines exactly */
        goto error;
    }

    RIL_onRequestComplete(t, RIL_E_SUCCESS, response, sizeof(response));
    at_response_free(p_response);
    return;

error:
    LOGE("requestOperator must not return error when radio is on");
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    at_response_free(p_response);
}

static void requestSendSMS(void *data, size_t datalen, RIL_Token t)
{
    int err;
    const char *smsc;
    const char *pdu;
    int tpLayerLength;
    char *cmd1, *cmd2;
    char *line;
    RIL_SMS_Response response;
    ATResponse *p_response = NULL;

    smsc = ((const char **)data)[0];
    pdu = ((const char **)data)[1];

    tpLayerLength = strlen(pdu)/2;

    // "NULL for default SMSC"
    if (smsc == NULL) {
        smsc= "00";
    }

    asprintf(&cmd1, "AT+CMGS=%d", tpLayerLength);
    asprintf(&cmd2, "%s%s", smsc, pdu);

    err = at_send_command_sms(cmd1, cmd2, "+CMGS:", &p_response);

    if (err != 0 || p_response->success == 0) goto error;

    memset(&response, 0, sizeof(response));

    /* fill in messageRef and ackPDU */
    line = p_response->p_intermediates->line;
    err = at_tok_start(&line);
    if (err != 0)
        goto out;
    err = at_tok_nextint(&line, &(response.messageRef));
out:
    response.ackPDU = NULL;

    RIL_onRequestComplete(t, RIL_E_SUCCESS, &response, sizeof(response));
    at_response_free(p_response);
    return;

error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    at_response_free(p_response);
}

static void requestSetupDataCall(void *data, size_t datalen, RIL_Token t)
{
    const char *apn;
    char *cmd;
    int err;
    ATResponse *p_response = NULL;
    char *response[3] = { "1", PPP_TTY_PATH, "0.0.0.0" };

    apn = ((const char **)data)[2];

    /* Configure DLC1 for GPRS data */
    err = at_send_command("AT+XDATACHANNEL=1,1,\"/mux/1\",\"/mux/2\",0", NULL);

#ifdef USE_TI_COMMANDS
    // Config for multislot class 10 (probably default anyway eh?)
    err = at_send_command("AT%CPRIM=\"GMM\",\"CONFIG MULTISLOT_CLASS=<10>\"",
                        NULL);

    err = at_send_command("AT%DATA=2,\"UART\",1,,\"SER\",\"UART\",0", NULL);
#endif /* USE_TI_COMMANDS */

    int fd, qmistatus;
    size_t cur = 0;
    size_t len;
    ssize_t written, rlen;
    char status[32] = {0};
    int retry = 10;
    char ppp_exit_code[PROPERTY_VALUE_MAX];
    static char address_ip[PROPERTY_VALUE_MAX];

    LOGD("requesting data connection to APN '%s'", apn);

    fd = open ("/dev/qmi", O_RDWR);
    if (fd >= 0) { /* the device doesn't exist on the emulator */

	    LOGD("opened the qmi device\n");
	    asprintf(&cmd, "up:%s", apn);
	    len = strlen(cmd);

	    while (cur < len) {
		    do {
	            written = write (fd, cmd + cur, len - cur);
	        } while (written < 0 && errno == EINTR);

	        if (written < 0) {
                LOGE("### ERROR writing to /dev/qmi");
                close(fd);
                free(cmd);
                goto error;
            }

            cur += written;
        }
        free(cmd);

        // wait for interface to come online

        do {
            sleep(1);
            do {
                rlen = read(fd, status, 31);
            } while (rlen < 0 && errno == EINTR);

            if (rlen < 0) {
                LOGE("### ERROR reading from /dev/qmi");
                close(fd);
                goto error;
            } else {
                status[rlen] = '\0';
                LOGD("### status: %s", status);
            }
        } while (strncmp(status, "STATE=up", 8) && strcmp(status, "online") && --retry);

        close(fd);

        if (retry == 0) {
            LOGE("### Failed to get data connection up\n");
	        goto error;
		}

        qmistatus = system("netcfg rmnet0 dhcp");

        LOGD("netcfg rmnet0 dhcp: status %d\n", qmistatus);

	    if (qmistatus < 0) goto error;

	} else {

        asprintf(&cmd, "AT+CGDCONT=1,\"IP\",\"%s\"", apn);
	    //FIXME check for error here
	    err = at_send_command(cmd, NULL);
	    free(cmd);

#if 0
	    // Set required QoS params to default
	    err = at_send_command("AT+CGQREQ=1", NULL);

	    // Set minimum QoS params to default
	    err = at_send_command("AT+CGQMIN=1", NULL);
#endif

	    // packet-domain event reporting
	    err = at_send_command("AT+CGEREP=2,1", NULL);

	    // Start data on PDP context 1
	    err = at_send_command("ATD*99***1#", &p_response);

	    if (err < 0 || p_response->success == 0) {
	        goto error;
	    }

#if 0
      // Start pppd to acquire dns/ip adresses
      err = property_set("ctl.start", "pppd_gprs");
      LOGD("starting service pppd_gprs...");
      if (err < 0) {
        LOGD("### error in starting service pppd_gprs: err %d", err);
        goto error;
      };

      sleep(10);
      err = property_get("net.gprs.ppp-exit", ppp_exit_code,"");
      if (err < 0) {
        LOGD("### error getting net.gprs.ppp-exit value: err %d", err);
        goto error;
      };
      if (!strcmp(ppp_exit_code, "")) {
        LOGD("PPP connect successfully");
        err = property_get("net.gprs.local-ip", address_ip ,"");
        if (err < 0) {
          LOGD("### error getting net.gprs.local-ip value: err %d", err);
          goto error;
        }
        response[2] = address_ip;
        LOGD("net.gprs.local-ip: %s", response[2]);
      }
      else {
        LOGD("### PPP exit with error: %s", ppp_exit_code);
        goto error;
      }
#else
	// Start pppd to acquire dns/ip adresses
	err = property_set("net.ppp", "start");
	LOGD("starting service pppd_gprs...");
	if (err < 0) {
		LOGD("### error in starting service pppd_gprs: err %d", err);
	goto error;
	};

	sleep(10);
	err = property_get("net.gprs.ppp-exit", ppp_exit_code,"");
	if (err < 0) {
		LOGD("### error getting net.gprs.ppp-exit value: err %d", err);
	goto error;
	};
	if (!strcmp(ppp_exit_code, "0")) {
	LOGD("PPP connect successfully");
	err = property_get("net.ppp0.local-ip", address_ip ,"");
		if (err < 0) {
			LOGD("### error getting net.ppp0.local-ip value: err %d", err);
		goto error;
		}
	response[2] = address_ip;
	LOGD("net.ppp0.local-ip: %s", response[2]);

	}
	else {
	LOGD("### PPP exit with error: %s", ppp_exit_code);
	goto error;
	}
#endif
    }
    
    RIL_onRequestComplete(t, RIL_E_SUCCESS, response, sizeof(response));
    at_response_free(p_response);
    return;
 
 error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    at_response_free(p_response);
}
static void requestDeactivateDeactivateDataCall(void *data, size_t datalen, RIL_Token t)
{
  int err;
  char * cmd;
  char * cid;
  ATResponse *p_response = NULL;

  err = property_set("net.ppp", "stop");
  LOGD("stopping service pppd_gprs...");
  if (err < 0) {
	  LOGD("### error in stopping service pppd_gprs: err %d", err);
  goto error;
  };

  cid = ((char **)data)[0];
  
  asprintf(&cmd, "AT+CGACT=0,%s", cid);
  
  err = at_send_command(cmd, &p_response);
  free(cmd);
  
  if (err < 0 || p_response->success == 0) {
    goto error;
  }

  RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
  at_response_free(p_response);
  return;
  
 error:
  RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
  at_response_free(p_response);
}

static void requestSMSAcknowledge(void *data, size_t datalen, RIL_Token t)
{
    int ackSuccess;
    int err;

    ackSuccess = ((int *)data)[0];

    if (ackSuccess == 1) {
        err = at_send_command("AT+CNMA=1", NULL);
    } else if (ackSuccess == 0)  {
        err = at_send_command("AT+CNMA=2", NULL);
    } else {
        LOGE("unsupported arg to RIL_REQUEST_SMS_ACKNOWLEDGE\n");
        goto error;
    }

    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
    return;

error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);

}

static void  requestSIM_IO(void *data, size_t datalen, RIL_Token t)
{
    ATResponse *p_response = NULL;
    RIL_SIM_IO_Response sr;
    int err;
    char *cmd = NULL;
    RIL_SIM_IO *p_args;
    char *line;
    
    memset(&sr, 0, sizeof(sr));

    p_args = (RIL_SIM_IO *)data;

    /* FIXME handle pin2 */

    if (p_args->data == NULL) {
        asprintf(&cmd, "AT+CRSM=%d,%d,%d,%d,%d",
                    p_args->command, p_args->fileid,
                    p_args->p1, p_args->p2, p_args->p3);
    } else {
        asprintf(&cmd, "AT+CRSM=%d,%d,%d,%d,%d,%s",
                    p_args->command, p_args->fileid,
                    p_args->p1, p_args->p2, p_args->p3, p_args->data);
    }

    err = at_send_command_singleline(cmd, "+CRSM:", &p_response);

    if (err < 0 || p_response->success == 0) {
        goto error;
    }

    line = p_response->p_intermediates->line;

    err = at_tok_start(&line);
    if (err < 0) goto error;

    err = at_tok_nextint(&line, &(sr.sw1));
    if (err < 0) goto error;

    err = at_tok_nextint(&line, &(sr.sw2));
    if (err < 0) goto error;

    if (at_tok_hasmore(&line)) {
        err = at_tok_nextstr(&line, &(sr.simResponse));
        if (err < 0) goto error;
    }

    RIL_onRequestComplete(t, RIL_E_SUCCESS, &sr, sizeof(sr));
    at_response_free(p_response);
    free(cmd);
    return;

error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    at_response_free(p_response);
    free(cmd);

}

static void  requestEnterSimPin(void*  data, size_t  datalen, RIL_Token  t)
{
    ATResponse   *p_response = NULL;
    int           err;
    char*         cmd = NULL;
    const char**  strings = (const char**)data;;

    if ( datalen == sizeof(char*) ) {
        asprintf(&cmd, "AT+CPIN=\"%s\"", strings[0]);
    } else if ( datalen == 2*sizeof(char*) ) {
        asprintf(&cmd, "AT+CPIN=\"%s\",\"%s\"", strings[0], strings[1]);
    } else
        goto error;

    err = at_send_command(cmd, &p_response);
    free(cmd);

    if (err < 0 || p_response->success == 0) {
error:
        RIL_onRequestComplete(t, RIL_E_PASSWORD_INCORRECT, NULL, 0);
    } else {
        RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);

	/* Notify that SIM is ready */
	setRadioState(RADIO_STATE_SIM_READY);
    }
    at_response_free(p_response);
}


static void requestSendUSSD(void *data, size_t datalen, RIL_Token t)
{
  ATResponse *p_response = NULL;
  int err = 0;
  int length = 0;
  char *line = NULL;
  char *ussdstring = NULL;
  char *string = NULL;
  char *cmd = NULL;
  int n = 0;
  int dcs = 0;

  ussdstring = (char *)data;
  length = strlen(ussdstring);

  asprintf(&cmd, "AT+CUSD=%d, \"%s\"", 1, ussdstring);
  err = at_send_command_singleline(cmd, "+CUSD:", &p_response);
  free(cmd);

  if(err < 0 || p_response->success == 0) goto error;

  line = p_response->p_intermediates->line;
  err = at_tok_start(&line);
  if(err < 0) goto error;

  err = at_tok_nextint(&line, &n);
  if(err < 0) goto error;

  err = at_tok_nextstr(&line, &string);
  if(err < 0) goto error;

  if (!at_tok_hasmore(&line)) goto end;

  err = at_tok_nextint(&line, &dcs);
  if(err < 0) goto error;

 end:
  at_response_free(p_response);
  RIL_onUnsolicitedResponse(RIL_UNSOL_ON_USSD, string, strlen(string));

  return;
error:
  at_response_free(p_response);
  RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
}


/*** Callback methods from the RIL library to us ***/

/**
 * Call from RIL to us to make a RIL_REQUEST
 *
 * Must be completed with a call to RIL_onRequestComplete()
 *
 * RIL_onRequestComplete() may be called from any thread, before or after
 * this function returns.
 *
 * Will always be called from the same thread, so returning here implies
 * that the radio is ready to process another command (whether or not
 * the previous command has completed).
 */
static void
onRequest (int request, void *data, size_t datalen, RIL_Token t)
{
    ATResponse *p_response;
    int err;

    LOGD("onRequest: %s", requestToString(request));

    /* Ignore all requests except RIL_REQUEST_GET_SIM_STATUS
     * when RADIO_STATE_UNAVAILABLE.
     */
    if (sState == RADIO_STATE_UNAVAILABLE
        && request != RIL_REQUEST_GET_SIM_STATUS
    ) {
        RIL_onRequestComplete(t, RIL_E_RADIO_NOT_AVAILABLE, NULL, 0);
        return;
    }

    /* Ignore all non-power requests when RADIO_STATE_OFF
     * (except RIL_REQUEST_GET_SIM_STATUS)
     */
    if (sState == RADIO_STATE_OFF
        && !(request == RIL_REQUEST_RADIO_POWER
            || request == RIL_REQUEST_GET_SIM_STATUS)
    ) {
        RIL_onRequestComplete(t, RIL_E_RADIO_NOT_AVAILABLE, NULL, 0);
        return;
    }

    switch (request) {
        case RIL_REQUEST_GET_SIM_STATUS: {
            RIL_CardStatus *p_card_status;
            char *p_buffer;
            int buffer_size;

            int result = getCardStatus(&p_card_status);
            if (result == RIL_E_SUCCESS) {
                p_buffer = (char *)p_card_status;
                buffer_size = sizeof(*p_card_status);
            } else {
                p_buffer = NULL;
                buffer_size = 0;
            }
            RIL_onRequestComplete(t, result, p_buffer, buffer_size);
            freeCardStatus(p_card_status);
            break;
        }
        case RIL_REQUEST_GET_CURRENT_CALLS:
            requestGetCurrentCalls(data, datalen, t);
            break;
        case RIL_REQUEST_DIAL:
            requestDial(data, datalen, t);
            break;
        case RIL_REQUEST_HANGUP:
            requestHangup(data, datalen, t);
            break;
        case RIL_REQUEST_HANGUP_WAITING_OR_BACKGROUND:
            // 3GPP 22.030 6.5.5
            // "Releases all held calls or sets User Determined User Busy
            //  (UDUB) for a waiting call."
            at_send_command("AT+CHLD=0", NULL);

            /* success or failure is ignored by the upper layer here.
               it will call GET_CURRENT_CALLS and determine success that way */
            RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
            break;
        case RIL_REQUEST_HANGUP_FOREGROUND_RESUME_BACKGROUND:
            // 3GPP 22.030 6.5.5
            // "Releases all active calls (if any exist) and accepts
            //  the other (held or waiting) call."
            at_send_command("AT+CHLD=1", NULL);

            /* success or failure is ignored by the upper layer here.
               it will call GET_CURRENT_CALLS and determine success that way */
            RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
            break;
        case RIL_REQUEST_SWITCH_WAITING_OR_HOLDING_AND_ACTIVE:
            // 3GPP 22.030 6.5.5
            // "Places all active calls (if any exist) on hold and accepts
            //  the other (held or waiting) call."
            at_send_command("AT+CHLD=2", NULL);

#ifdef WORKAROUND_ERRONEOUS_ANSWER
            s_expectAnswer = 1;
#endif /* WORKAROUND_ERRONEOUS_ANSWER */

            /* success or failure is ignored by the upper layer here.
               it will call GET_CURRENT_CALLS and determine success that way */
            RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
            break;
        case RIL_REQUEST_ANSWER:
            at_send_command("ATA", NULL);

#ifdef WORKAROUND_ERRONEOUS_ANSWER
            s_expectAnswer = 1;
#endif /* WORKAROUND_ERRONEOUS_ANSWER */

            /* success or failure is ignored by the upper layer here.
               it will call GET_CURRENT_CALLS and determine success that way */
            RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
            break;
        case RIL_REQUEST_CONFERENCE:
            // 3GPP 22.030 6.5.5
            // "Adds a held call to the conversation"
            at_send_command("AT+CHLD=3", NULL);

            /* success or failure is ignored by the upper layer here.
               it will call GET_CURRENT_CALLS and determine success that way */
            RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
            break;
        case RIL_REQUEST_UDUB:
            /* user determined user busy */
            /* sometimes used: ATH */
            at_send_command("ATH", NULL);

            /* success or failure is ignored by the upper layer here.
               it will call GET_CURRENT_CALLS and determine success that way */
            RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
            break;

        case RIL_REQUEST_SEPARATE_CONNECTION:
            {
                char  cmd[12];
                int   party = ((int*)data)[0];

                // Make sure that party is in a valid range.
                // (Note: The Telephony middle layer imposes a range of 1 to 7.
                // It's sufficient for us to just make sure it's single digit.)
                if (party > 0 && party < 10) {
                    sprintf(cmd, "AT+CHLD=2%d", party);
                    at_send_command(cmd, NULL);
                    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
                } else {
                    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
                }
            }
            break;

        case RIL_REQUEST_SIGNAL_STRENGTH:
            requestSignalStrength(data, datalen, t);
            break;
        case RIL_REQUEST_REGISTRATION_STATE:
        case RIL_REQUEST_GPRS_REGISTRATION_STATE:
            requestRegistrationState(request, data, datalen, t);
            break;

      case RIL_REQUEST_SET_MUTE:
        requestSetMute(data, datalen, t);
        break;

      case RIL_REQUEST_GET_MUTE:
        requestGetMute(data, datalen, t);
        break;

      case RIL_REQUEST_SCREEN_STATE:
        requestScreenState(data, datalen, t);
        break;

        case RIL_REQUEST_OPERATOR:
            requestOperator(data, datalen, t);
            break;
        case RIL_REQUEST_RADIO_POWER:
            requestRadioPower(data, datalen, t);
            break;

      case RIL_REQUEST_QUERY_FACILITY_LOCK:
        requestQueryFacilityLock(data, datalen, t);
        break;

      case RIL_REQUEST_DTMF_STOP:
        requestDtmfStop(data, datalen, t);
        break;

      case RIL_REQUEST_DTMF_START:
        requestDtmfStart(data, datalen, t);
        break;

        case RIL_REQUEST_DTMF: {
            char c = ((char *)data)[0];
            char *cmd;
            asprintf(&cmd, "AT+VTS=%c", (int)c);
            at_send_command(cmd, NULL);
            free(cmd);
            RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
            break;
        }
        case RIL_REQUEST_SEND_SMS:
            requestSendSMS(data, datalen, t);
            break;
        case RIL_REQUEST_SETUP_DATA_CALL:
            requestSetupDataCall(data, datalen, t);
            break;

      case RIL_REQUEST_DEACTIVATE_DATA_CALL:
        requestDeactivateDeactivateDataCall(data, datalen, t);
        break;

        case RIL_REQUEST_SMS_ACKNOWLEDGE:
            requestSMSAcknowledge(data, datalen, t);
            break;

        case RIL_REQUEST_GET_IMSI:
            p_response = NULL;
            err = at_send_command_numeric("AT+CIMI", &p_response);

            if (err < 0 || p_response->success == 0) {
                RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
            } else {
                RIL_onRequestComplete(t, RIL_E_SUCCESS,
                    p_response->p_intermediates->line, sizeof(char *));
            }
            at_response_free(p_response);
            break;

        case RIL_REQUEST_BASEBAND_VERSION:
            requestBasebandVersion(data, datalen, t);
            break;

        case RIL_REQUEST_GET_IMEI:
        case RIL_REQUEST_GET_IMEISV:
            p_response = NULL;
            err = at_send_command_numeric("AT+CGSN", &p_response);

            if (err < 0 || p_response->success == 0) {
                RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
            } else {
                RIL_onRequestComplete(t, RIL_E_SUCCESS,
                    p_response->p_intermediates->line, sizeof(char *));
            }
            at_response_free(p_response);
            break;

      case RIL_REQUEST_QUERY_AVAILABLE_NETWORKS:
        requestQueryAvailableNetworks(data, datalen, t);
        break;

      case RIL_REQUEST_GET_PREFERRED_NETWORK_TYPE:
        requestGetPreferredNetworkType(data, datalen, t);
        break;

      case RIL_REQUEST_SET_PREFERRED_NETWORK_TYPE:
        requestSetPreferredNetworkType(data, datalen, t);
        break;

        case RIL_REQUEST_GET_NEIGHBORING_CELL_IDS:
            requestNeighboringCellIds(data, datalen, t);
            break;

        case RIL_REQUEST_SIM_IO:
            requestSIM_IO(data,datalen,t);
            break;

        case RIL_REQUEST_SEND_USSD:
            requestSendUSSD(data, datalen, t);
            break;

        case RIL_REQUEST_CANCEL_USSD:
            p_response = NULL;
            err = at_send_command_numeric("AT+CUSD=2", &p_response);

            if (err < 0 || p_response->success == 0) {
                RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
            } else {
                RIL_onRequestComplete(t, RIL_E_SUCCESS,
                    p_response->p_intermediates->line, sizeof(char *));
            }
            at_response_free(p_response);
            break;

        case RIL_REQUEST_SET_NETWORK_SELECTION_AUTOMATIC:
            err = at_send_command("AT+COPS=0", NULL);
            if(err < 0)
              RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
            else
              RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
            break;

        case RIL_REQUEST_DATA_CALL_LIST:
            requestDataCallList(data, datalen, t);
            break;

        case RIL_REQUEST_QUERY_NETWORK_SELECTION_MODE:
            requestQueryNetworkSelectionMode(data, datalen, t);
            break;

        case RIL_REQUEST_OEM_HOOK_RAW:
            // echo back data
            RIL_onRequestComplete(t, RIL_E_SUCCESS, data, datalen);
            break;


        case RIL_REQUEST_OEM_HOOK_STRINGS: {
            int i;
            const char ** cur;

            LOGD("got OEM_HOOK_STRINGS: 0x%8p %lu", data, (long)datalen);


            for (i = (datalen / sizeof (char *)), cur = (const char **)data ;
                    i > 0 ; cur++, i --) {
                LOGD("> '%s'", *cur);
            }

            // echo back strings
            RIL_onRequestComplete(t, RIL_E_SUCCESS, data, datalen);
            break;
        }

        case RIL_REQUEST_WRITE_SMS_TO_SIM:
            requestWriteSmsToSim(data, datalen, t);
            break;

        case RIL_REQUEST_DELETE_SMS_ON_SIM: {
            char * cmd;
            p_response = NULL;
            asprintf(&cmd, "AT+CMGD=%d", ((int *)data)[0]);
            err = at_send_command(cmd, &p_response);
            free(cmd);
            if (err < 0 || p_response->success == 0) {
                RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
            } else {
                RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
            }
            at_response_free(p_response);
            break;
        }

        case RIL_REQUEST_ENTER_SIM_PIN:
        case RIL_REQUEST_ENTER_SIM_PUK:
        case RIL_REQUEST_ENTER_SIM_PIN2:
        case RIL_REQUEST_ENTER_SIM_PUK2:
        case RIL_REQUEST_CHANGE_SIM_PIN:
        case RIL_REQUEST_CHANGE_SIM_PIN2:
            requestEnterSimPin(data, datalen, t);
            break;

      case RIL_REQUEST_QUERY_CALL_WAITING:
        requestQueryCallWaiting(data, datalen, t);
        break;

      case RIL_REQUEST_SET_CALL_WAITING:
        requestSetCallWaiting(data, datalen, t);
        break;

      case RIL_REQUEST_QUERY_CALL_FORWARD_STATUS:
        requestQueryCallForwardStatus(t);
        break;

      case RIL_REQUEST_SET_CALL_FORWARD:
        requestSetCallForward(data, t);
        break;

      case RIL_REQUEST_GET_CLIR:
        requestGetCLIR(data, datalen, t);
        break;

      case RIL_REQUEST_SET_CLIR:
        requestSetCLIR(data, datalen, t);
        break;

      case RIL_REQUEST_SEND_SMS_EXPECT_MORE:
        requestSendSMSExpectMore(data, datalen, t);
        break;

      case RIL_REQUEST_ENTER_NETWORK_DEPERSONALIZATION:
        // NOTE: There isn't an AT command with this capability
        requestNotSupported(t);
        break;

      case RIL_REQUEST_SET_FACILITY_LOCK:
       requestSetFacilityLock(data, datalen, t);
        break;

      case RIL_REQUEST_CHANGE_BARRING_PASSWORD:
        requestChangeBarringPassword(data, datalen, t);
        break;

      case RIL_REQUEST_SET_NETWORK_SELECTION_MANUAL:
        requestSetNetworkSelectionManual(data, datalen, t);
        break;

      case RIL_REQUEST_QUERY_CLIP:
        requestQueryCLIP(data, datalen, t);
        break;

      case RIL_REQUEST_RESET_RADIO:
        requestResetRadio(t);
        break;

     case RIL_REQUEST_SET_SUPP_SVC_NOTIFICATION:
        requestSetSuppSVCNotification(data, datalen, t);
        break;

      case RIL_REQUEST_EXPLICIT_CALL_TRANSFER:
        requestExplicitCallTransfer(t);
        break;

      case RIL_REQUEST_SET_LOCATION_UPDATES:
        requestSetLocationUpdates(data, datalen, t);
        break;

      case RIL_REQUEST_STK_GET_PROFILE:
        requestSTKGetprofile(t);
        break;

      case RIL_REQUEST_STK_SET_PROFILE:
        requestSTKSetProfile(data, datalen, t);
        break;

      case RIL_REQUEST_LAST_DATA_CALL_FAIL_CAUSE:
      case RIL_REQUEST_LAST_CALL_FAIL_CAUSE:
        requestLastFailCause(t);
        break;

      case RIL_REQUEST_STK_SEND_ENVELOPE_COMMAND:
        requestSTKSendEnvelopeCommand(data, datalen, t);
        break;

      case RIL_REQUEST_STK_SEND_TERMINAL_RESPONSE:
        requestSTKSendTerminalResponse(data, datalen, t);
        break;

      default:
        requestNotSupported(t);
        break;
    }
}


static void unsolicitedNitzTime(const char * s)
{
  int err;
  char * response = NULL;
  char * line = NULL;
  char * p = NULL;
  char * tz = NULL; /* Timezone */
  line = strdup(s);

  /* Higher layers expect a NITZ string in this format:
   *  08/10/28,19:08:37-20,1 (yy/mm/dd,hh:mm:ss(+/-)tz,dst)
   */

  if(strStartsWith(s,"+CTZV:")){
    
    /* Get Time and Timezone data and store in static variable.
     * Wait until DST is received to send response to upper layers
     */
    at_tok_start(&line);
    
    err = at_tok_nextstr(&line, &tz);
    if (err < 0) goto error;
    
    err = at_tok_nextstr(&line, &response);
    if (err < 0) goto error;
    
    strcat(response,tz);
    
    sNITZtime = response;
    return;
    
  }
  else if(strStartsWith(s,"+CTZDST:")){
    
    /* We got DST, now assemble the response and send to upper layers */
    at_tok_start(&line);

    err = at_tok_nextstr(&line, &tz);
    if (err < 0) goto error;
    
    asprintf(&response, "%s,%s", sNITZtime, tz);

    RIL_onUnsolicitedResponse(RIL_UNSOL_NITZ_TIME_RECEIVED, response, strlen(response));
    free(response);
    return;
    
  }else
    {
      goto error;
    }
    
 error:
  LOGE("Invalid NITZ line %s\n", s);
}

static void unsolicitedRSSI(const char * s)
{
  int err;
  int response[2];
  char * line = NULL;

  line = strdup(s);
  
  at_tok_start(&line);

  err = at_tok_nextint(&line, &(at_csq_rssi));  
  if (err < 0) goto error;  

  response[0] = XGoldRSSITo3gpp(at_csq_rssi);
  response[1] = at_csq_ber;

  RIL_onUnsolicitedResponse(RIL_UNSOL_SIGNAL_STRENGTH, response, sizeof(response));
  return;
  
 error:
  /* The notification was for a battery event - do not send a msg to upper layers */
  return;
}

static void requestNotSupported(RIL_Token t)
{
  RIL_onRequestComplete(t, RIL_E_REQUEST_NOT_SUPPORTED, NULL, 0);
  return;
}

static void requestQueryCallWaiting(void *data, size_t datalen, RIL_Token t)
{
  ATResponse *p_response = NULL;
  int err;
  char *cmd = NULL;
  char *line;
  int class;
  int response[2];
  
  class = ((int *)data)[0];
  
  asprintf(&cmd, "AT+CCWA=1,2,%d",class);
  
  err = at_send_command_singleline(cmd, "+CCWA:", &p_response);
  
  if (err < 0 || p_response->success == 0) goto error;
  
  line = p_response->p_intermediates->line;
  
  err = at_tok_start(&line);
  if (err < 0) goto error;
  
  err = at_tok_nextint(&line, &(response[0]));
  if (err < 0) goto error;
  
  if (at_tok_hasmore(&line)) {
    err = at_tok_nextint(&line, &(response[1]));
    if (err < 0) goto error;
  }
  
  RIL_onRequestComplete(t, RIL_E_SUCCESS, response, sizeof(response));
  at_response_free(p_response);
  free(cmd);
  return;
  
 error:
  RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
  at_response_free(p_response);
  free(cmd);
}

static void requestSetCallWaiting(void *data, size_t datalen, RIL_Token t)
{
  ATResponse *p_response = NULL;
  int err;
  char *cmd = NULL;
  int enabled, class;
  
  if((datalen<2)||(data==NULL)) goto error;
  
  enabled = ((int *)data)[0];
  class = ((int *)data)[1];
  
  asprintf(&cmd, "AT+CCWA=0,%d,%d",enabled,class);
  
  err = at_send_command(cmd, NULL);
  
  if (err < 0 ) goto error;
  
  RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
  at_response_free(p_response);
  free(cmd);
  return;
  
 error:
  RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
  at_response_free(p_response);
  free(cmd);
}

static void requestQueryCallForwardStatus(RIL_Token t)
{
  int err = 0;
  int i = 0;
  int n = 0;
  int tmp = 0;
  ATResponse *p_response = NULL;
  ATLine *p_cur;
  RIL_CallForwardInfo **responses = NULL;
  char *cmd;
  
  err = at_send_command_multiline("AT+CCFC=0,2", "+CCFC:", &p_response);
  
  if(err != 0 || p_response->success == 0)
    goto error;
  
  for (p_cur = p_response->p_intermediates; p_cur != NULL; p_cur = p_cur->p_next)
    n++;
  
  responses = alloca(n * sizeof(RIL_CallForwardInfo *));
  
  for(i = 0; i < n; i++) {
    responses[i] = alloca(sizeof(RIL_CallForwardInfo));
    responses[i]->status = 0;
    responses[i]->reason = 0;
    responses[i]->serviceClass = 0;
    responses[i]->toa = 0;
    responses[i]->number = "";
    responses[i]->timeSeconds = 0;
  }
  
  for (i = 0,p_cur = p_response->p_intermediates; p_cur != NULL; p_cur = p_cur->p_next, i++) {
    char *line = p_cur->line;
    
    err = at_tok_start(&line);
    if (err < 0) goto error;
    
    err = at_tok_nextint(&line, &(responses[i]->status));
    if (err < 0) goto error;
    
    err = at_tok_nextint(&line, &(responses[i]->serviceClass));
    if (err < 0) goto error;
    
    if(!at_tok_hasmore(&line)) continue;
    
    err = at_tok_nextstr(&line, &(responses[i]->number));
    if (err < 0) goto error;
    
    if(!at_tok_hasmore(&line)) continue;
    
    err = at_tok_nextint(&line, &(responses[i]->toa));
    if (err < 0) goto error;
    
    if(!at_tok_hasmore(&line)) continue;
    at_tok_nextint(&line,&tmp);
    
    if(!at_tok_hasmore(&line)) continue;
    at_tok_nextint(&line,&tmp);
    
    if(!at_tok_hasmore(&line)) continue;
    err = at_tok_nextint(&line, &(responses[i]->timeSeconds));
    if (err < 0) goto error;
    
  }
  
  at_response_free(p_response);
  RIL_onRequestComplete(t, RIL_E_SUCCESS, responses, sizeof(RIL_CallForwardInfo **));
  return;
  
 error:
  at_response_free(p_response);
  RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
}

static void requestSetCallForward(void *data, RIL_Token t)
{
  int err = 0;
  char *cmd = NULL;
  RIL_CallForwardInfo *info = NULL;
  
  info = ((RIL_CallForwardInfo *) data);
  
  if(data == NULL) goto error;
  
  asprintf(&cmd, "AT+CCFC = %d, %d, \"%s\"",
	   info->reason, info->status,
	   info->number);
  
  err = at_send_command(cmd, NULL);
  
  if (err < 0 ) goto error;
  
  RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
  free(cmd);
  return;
  
 error:
  RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
  free(cmd);
}

static void requestGetCLIR(void *data, size_t datalen, RIL_Token t)
{
  /* Even though data and datalen must be NULL and 0 respectively this
   * condition is not verified
   */
  ATResponse *p_response = NULL;
  int response[2];
  char *line = NULL;
  int err = 0;
  
  err = at_send_command_singleline("AT+CLIR?", "+CLIR:", &p_response);
  
  if (err < 0 || p_response->success == 0) goto error;
  
  line = p_response->p_intermediates->line;
  err = at_tok_start(&line);
  if (err < 0) goto error;
  
  err = at_tok_nextint(&line, &(response[0]));
  if (err < 0) goto error;
  
  err = at_tok_nextint(&line, &(response[1]));
  if (err < 0) goto error;
  
  RIL_onRequestComplete(t, RIL_E_SUCCESS, response, sizeof(response));
  at_response_free(p_response);
  return;
  
 error:
  RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
  at_response_free(p_response);
}

static void requestSetCLIR(void *data, size_t datalen, RIL_Token t)
{
  char *cmd = NULL;
  int err = 0;
  
  asprintf(&cmd, "AT+CLIR=%d", ((int *)data)[0]);
  
  err = at_send_command(cmd, NULL);
  free(cmd);
  
  if (err < 0)
    RIL_onRequestComplete(t, RIL_E_PASSWORD_INCORRECT, NULL, 0);
  else
    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
}

static void requestSendSMSExpectMore(void *data, size_t datalen, RIL_Token t)
{
  char *cmd = NULL;
  asprintf(&cmd, "AT+CMMS=1");
  at_send_command(cmd, NULL);
  free(cmd);
  requestSendSMS(data, datalen, t);
}

static void requestSetFacilityLock(void *data, size_t datalen, RIL_Token t)
{
  /* It must be tested if the Lock for a particular class can be set without
   * modifing the values of the other class. If not, first must call
   * requestQueryFacilityLock to obtain the previus value
   */
  int err = 0;
  char *cmd = NULL;
  char *code = NULL;
  char *lock = NULL;
  char *password = NULL;
  char *class = NULL;
  
  assert (datalen >=  (4 * sizeof(char **)));
  
  code = ((char **)data)[0];
  lock = ((char **)data)[1];
  password = ((char **)data)[2];
  class = ((char **)data)[3];
  
  asprintf(&cmd, "AT+CLCK=\"%s\",%s,\"%s\",%s", code, lock, password, class);
  err = at_send_command(cmd, NULL);
  if (err < 0) goto error;
  
  free(cmd);
  RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
  return;

 error:
  RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
}

static void requestChangeBarringPassword(void *data, size_t datalen, RIL_Token t)
{
  int err = 0;
  char *cmd = NULL;
  char *string = NULL;
  char *old_password = NULL;
  char *new_password = NULL;
  
  assert (datalen >=  (3 * sizeof(char **)));
  
  string	   = ((char **)data)[0];
  old_password = ((char **)data)[1];
  new_password = ((char **)data)[2];
  
  asprintf(&cmd, "AT+CPWD=\"%s\",\"%s\",\"%s\"", string, old_password, new_password);
  err = at_send_command(cmd, NULL);
  
  free(cmd);
  
  if (err < 0) goto error;
  
  RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
  return;
 
error:
  RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
}

static void requestSetNetworkSelectionManual(void *data, size_t datalen, RIL_Token t)
{
  char *operator = NULL;
  char *cmd = NULL;
  int err = 0;
  ATResponse *p_response = NULL;
  
  operator = (char *)data;
  asprintf(&cmd, "AT+COPS=1,2,\"%s\"", operator);
  err = at_send_command(cmd, &p_response);
  free(cmd);
  if (err < 0 || p_response->success == 0){
    err = at_send_command("AT+COPS=0", NULL);
		if(err < 0) goto error;
  }
  
  RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
  at_response_free(p_response);

  return;
 
error:
  at_response_free(p_response);
  RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
}

static void requestQueryCLIP(void *data, size_t datalen, RIL_Token t)
{
  ATResponse *p_response = NULL;
  int err;
  char *line;
  int response;
  
  err = at_send_command_singleline("AT+CLIP?","+CLIP:",&p_response);
  if(err < 0 || p_response->success == 0) goto error;
  
  line = p_response->p_intermediates->line;
  err = at_tok_start(&line);
  if(err < 0) goto error;
  
  /* The first number is discarded */
  err = at_tok_nextint(&line, &response);
  if(err < 0) goto error;
  
  err = at_tok_nextint(&line, &response);
  if(err < 0) goto error;
  
  RIL_onRequestComplete(t, RIL_E_SUCCESS, &response, sizeof(int));
  at_response_free(p_response);
  return;

 error:
  at_response_free(p_response);
  RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
}

static void requestResetRadio(RIL_Token t)
{
  int err = 0;
  
  err = at_send_command("AT+CFUN=16", NULL);
  if(err < 0)
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
  else
    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
  
  return;
}

static void requestSetSuppSVCNotification(void *data, size_t datalen, RIL_Token t)
{
  int err = 0;
  int enabled = 0;
  char *cmd = NULL;
  enabled = ((int *)data)[0];
  
  asprintf(&cmd, "AT+CSSN=%d,%d", enabled, enabled);
  err = at_send_command(cmd,NULL);
  free(cmd);
  if(err < 0)
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
  else
    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
}

static void requestExplicitCallTransfer(RIL_Token t)
{
  int err = 0;
  err = at_send_command("AT+CHLD=4",NULL);
  if(err < 0)
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
  else
    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
}

static void requestSetLocationUpdates(void *data, size_t datalen, RIL_Token t)
{
  int err = 0;
  int updates = 0;
  char *cmd = NULL;
  ATResponse *p_response = NULL;
  updates = ((int *)data)[0] == 1? 2 : 1;
  
  asprintf(&cmd, "AT+CREG=%d", updates);
  
  err = at_send_command_singleline(cmd,"+CLIP:",&p_response);
  free(cmd);
  if(err < 0 || p_response->success == 0) goto error;
  
  RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
  at_response_free(p_response);
  return;
 
error:
  at_response_free(p_response);
  RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
}

static void requestSTKGetprofile(RIL_Token t)
{
  int err = 0;
  int responselen = 0;
  ATResponse *p_response = NULL;
  char *response = NULL;
  char *line = NULL;
  
  err = at_send_command_singleline("AT+STKPROF?", "+STKPROF:", &p_response);
  
  if(err < 0 || p_response->success == 0) goto error;
  
  line = p_response->p_intermediates->line;
  err = at_tok_start(&line);
  if(err < 0) goto error;
  
  err = at_tok_nextint(&line, &responselen);
  if(err < 0) goto error;
  
  err = at_tok_nextstr(&line, &response);
  if(err < 0) goto error;
  
  at_response_free(p_response);
  RIL_onRequestComplete(t, RIL_E_SUCCESS, response, responselen * sizeof(char));
  return;

 error:
  at_response_free(p_response);
  RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
  return;
}

static void requestSTKSetProfile(void * data, size_t datalen, RIL_Token t)
{
  int err = 0;
  int length = 0;
  char *profile = NULL;
  char *cmd = NULL;
  
  profile = (char *)data;
  length = strlen(profile);
  asprintf(&cmd, "AT+STKPROF=%d,\"%s\"", length, profile);
  
  err = at_send_command(cmd, NULL);
  free(cmd);
  if(err < 0)
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
  else
    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
  return;
}

static void requestLastFailCause(RIL_Token t)
{
  ATResponse *p_response = NULL;
  int err = 0;
  int response = 0;
  char *tmp = NULL;
  char *line = NULL;
  
  err = at_send_command_singleline("AT+CEER", "+CEER:", &p_response);
  if(err < 0 || p_response->success == 0) goto error;
  
  line = p_response->p_intermediates->line;
  err = at_tok_start(&line);
  if(err < 0) goto error;
  
  err = at_tok_nextstr(&line, &tmp);
  if(err < 0) goto error;
  
  err = at_tok_nextint(&line, &response);
  if(err < 0) goto error;
  
  RIL_onRequestComplete(t, RIL_E_SUCCESS, &response, sizeof(int));
  at_response_free(p_response);
  return;
  
 error:
  at_response_free(p_response);
  RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
}

static void requestOEMHookStrings(void * data, size_t datalen, RIL_Token t)
{
  int i;
  const char ** cur;
  
  LOGD("got OEM_HOOK_STRINGS: 0x%8p %lu", data, (long)datalen);
  
  for (i = (datalen / sizeof (char *)), cur = (const char **)data; i > 0; cur++, i --){
    LOGD("> '%s'", *cur);
  }
  
  // echo back strings	
  RIL_onRequestComplete(t, RIL_E_SUCCESS, data, datalen);
  return;
}

static void requestSTKSendEnvelopeCommand(void * data, size_t datalen, RIL_Token t) {
  int err = 0;
  int len = 0;
  int envcmd = 0;
  int itemid = 0;
  int helpreq = 0;
  int eventlst = 0;
  int lang_cause = 0;
  char *hexdata = (char *)data;
  char *cmd = NULL;
  unsigned int *intdata = NULL;
  
  len = strlen(data);
  
  intdata = (unsigned int*)(alloca((len / 2) * sizeof(unsigned int)));
  
  HexStr_to_DecInt(data, intdata);
  
  envcmd = intdata[0];
  if(envcmd == 211){
    itemid = intdata[8];
    helpreq = intdata[9];
    asprintf(&cmd, "AT+STKENV=%d, %d, %d", envcmd, itemid, helpreq);
    err = at_send_command(cmd, NULL);
    if(err < 0)
      goto error;
  }
  else if(envcmd == 214){
    len = intdata[1];
    eventlst = intdata[4];
    if(len > 7) lang_cause = intdata[9];
    asprintf(&cmd, "AT+STKENV=%d, %d, %d", envcmd, eventlst, lang_cause);
    err = at_send_command(cmd, NULL);
    if(err < 0)
      goto error;
    
  }
  else{
    goto notsupported;
  }
  
  free(cmd);
  free(intdata);
  RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
  return;

 notsupported:
  free(cmd);
  free(intdata);
  RIL_onRequestComplete(t, RIL_E_REQUEST_NOT_SUPPORTED, NULL, 0);
  return;
  
 error:
  free(cmd);
  free(intdata);
  RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
  return;
}

static void requestSTKSendTerminalResponse(void * data, size_t datalen, RIL_Token t) {
  int err = 0;
  int len = 0;
  int command = 0;
  int result = 0;
  int additionalInfo = 0;
  int dcs = 0;
  int offset = 0;
  int optInfoLen = 0;
  char *optInfo = NULL;
  int i = 0;
  char *hexdata = (char *)data;
  char *cmd = NULL;
  unsigned int *intdata = NULL;
  
  len = strlen(data);
  intdata = (unsigned int*)(alloca((len / 2) * sizeof(unsigned int)));
  HexStr_to_DecInt(data, intdata);
  command = intdata[2];
  
  switch(command){
  case 21:
    command = 33;
    break;
  case 20:
    command = 32;
    break;
  case 15:
    command = 21;
    break;
  case 22:
    command = 34;
    break;
  case 23:
    command = 35;
    break;
  case 24:
    command = 36;
    break;
  default:
    goto notsupported;
    break;
  }
  
  switch(command){
  case 32:
  case 33:{
    result = intdata[11];
    if(intdata[10] > 1)
      additionalInfo = intdata[12];
    asprintf(&cmd, "AT+STKTR=%d, %d, %d", command, result, additionalInfo);
    err = at_send_command(cmd, NULL);
    if(err < 0)
      goto error;
    break;
  }
  case 21:{
    result = intdata[11];
    asprintf(&cmd, "AT+STKTR=%d, %d", command, result);
    err = at_send_command(cmd, NULL);
    if(err < 0)
      goto error;
    break;
  }
  case 34:
  case 35:
  case 36:{
    result = intdata[11];
    if(intdata[10] > 1){
      additionalInfo = intdata[12];
      offset = 1;
    }
    optInfoLen = (intdata[13] + offset) * 2;
    optInfo = alloca(optInfoLen * sizeof(char));
    for(i = 0; i < optInfoLen; i++)
                                optInfo[i] = hexdata[15 + offset + i];
    
    asprintf(&cmd, "AT+STKTR=%d, %d, %d, 0, %d,\"%s\"", command, result, additionalInfo, intdata[14+offset], optInfo);
    
    err = at_send_command(cmd, NULL);
    if(err < 0)
      goto error;
    
    break;
  }
  }
  
  free(cmd);
  free(intdata);
  free(optInfo);
  RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
  return;
  
 notsupported:
  free(cmd);
  free(intdata);
  free(optInfo);
  RIL_onRequestComplete(t, RIL_E_REQUEST_NOT_SUPPORTED, NULL, 0);
  return;
  
 error:
  free(cmd);
  free(intdata);
  free(optInfo);
  RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
}

/**
 * Synchronous call from the RIL to us to return current radio state.
 * RADIO_STATE_UNAVAILABLE should be the initial state.
 */
static RIL_RadioState
currentState()
{
    return sState;
}
/**
 * Call from RIL to us to find out whether a specific request code
 * is supported by this implementation.
 *
 * Return 1 for "supported" and 0 for "unsupported"
 */

static int
onSupports (int requestCode)
{
    //@@@ todo

    return 1;
}

static void onCancel (RIL_Token t)
{
    //@@@todo

}

static const char * getVersion(void)
{
    LOGD("Vendor RIL Build date: %s %s\n", __DATE__, __TIME__);
    return "android infineon xgold-ril 1.0";
}

static void
setRadioState(RIL_RadioState newState)
{
    RIL_RadioState oldState;

    pthread_mutex_lock(&s_state_mutex);

    oldState = sState;

    if (s_closed > 0) {
        // If we're closed, the only reasonable state is
        // RADIO_STATE_UNAVAILABLE
        // This is here because things on the main thread
        // may attempt to change the radio state after the closed
        // event happened in another thread
        newState = RADIO_STATE_UNAVAILABLE;
    }

    if (sState != newState || s_closed > 0) {
        sState = newState;

        pthread_cond_broadcast (&s_state_cond);
    }

    pthread_mutex_unlock(&s_state_mutex);

    /* do these outside of the mutex */
    if (sState != oldState) {
        RIL_onUnsolicitedResponse (RIL_UNSOL_RESPONSE_RADIO_STATE_CHANGED,
                                    NULL, 0);

        /* FIXME onSimReady() and onRadioPowerOn() cannot be called
         * from the AT reader thread
         * Currently, this doesn't happen, but if that changes then these
         * will need to be dispatched on the request thread
         */
        if (sState == RADIO_STATE_SIM_READY) {
            onSIMReady();
        } else if (sState == RADIO_STATE_SIM_NOT_READY) {
            onRadioPowerOn();
        }
    }
}

/** Returns SIM_NOT_READY on error */
static SIM_Status 
getSIMStatus()
{
    ATResponse *p_response = NULL;
    int err;
    int ret;
    char *cpinLine;
    char *cpinResult;

    if (sState == RADIO_STATE_OFF || sState == RADIO_STATE_UNAVAILABLE) {
        ret = SIM_NOT_READY;
        goto done;
    }

    err = at_send_command_singleline("AT+CPIN?", "+CPIN:", &p_response);

    if (err != 0) {
        ret = SIM_NOT_READY;
        goto done;
    }

    switch (at_get_cme_error(p_response)) {
        case CME_SUCCESS:
            break;

        case CME_SIM_NOT_INSERTED:
            ret = SIM_ABSENT;
            goto done;

        default:
            ret = SIM_NOT_READY;
            goto done;
    }

    /* CPIN? has succeeded, now look at the result */

    cpinLine = p_response->p_intermediates->line;
    err = at_tok_start (&cpinLine);

    if (err < 0) {
        ret = SIM_NOT_READY;
        goto done;
    }

    err = at_tok_nextstr(&cpinLine, &cpinResult);

    if (err < 0) {
        ret = SIM_NOT_READY;
        goto done;
    }

    if (0 == strcmp (cpinResult, "SIM PIN")) {
        ret = SIM_PIN;
        goto done;
    } else if (0 == strcmp (cpinResult, "SIM PUK")) {
        ret = SIM_PUK;
        goto done;
    } else if (0 == strcmp (cpinResult, "PH-NET PIN")) {
        return SIM_NETWORK_PERSONALIZATION;
    } else if (0 != strcmp (cpinResult, "READY"))  {
        /* we're treating unsupported lock types as "sim absent" */
        ret = SIM_ABSENT;
        goto done;
    }

    at_response_free(p_response);
    p_response = NULL;
    cpinResult = NULL;

    ret = SIM_READY;

done:
    at_response_free(p_response);
    return ret;
}


/**
 * Get the current card status.
 *
 * This must be freed using freeCardStatus.
 * @return: On success returns RIL_E_SUCCESS
 */
static int getCardStatus(RIL_CardStatus **pp_card_status) {
    static RIL_AppStatus app_status_array[] = {
        // SIM_ABSENT = 0
        { RIL_APPTYPE_UNKNOWN, RIL_APPSTATE_UNKNOWN, RIL_PERSOSUBSTATE_UNKNOWN,
          NULL, NULL, 0, RIL_PINSTATE_UNKNOWN, RIL_PINSTATE_UNKNOWN },
        // SIM_NOT_READY = 1
        { RIL_APPTYPE_SIM, RIL_APPSTATE_DETECTED, RIL_PERSOSUBSTATE_UNKNOWN,
          NULL, NULL, 0, RIL_PINSTATE_UNKNOWN, RIL_PINSTATE_UNKNOWN },
        // SIM_READY = 2
        { RIL_APPTYPE_SIM, RIL_APPSTATE_READY, RIL_PERSOSUBSTATE_READY,
          NULL, NULL, 0, RIL_PINSTATE_UNKNOWN, RIL_PINSTATE_UNKNOWN },
        // SIM_PIN = 3
        { RIL_APPTYPE_SIM, RIL_APPSTATE_PIN, RIL_PERSOSUBSTATE_UNKNOWN,
          NULL, NULL, 0, RIL_PINSTATE_ENABLED_NOT_VERIFIED, RIL_PINSTATE_UNKNOWN },
        // SIM_PUK = 4
        { RIL_APPTYPE_SIM, RIL_APPSTATE_PUK, RIL_PERSOSUBSTATE_UNKNOWN,
          NULL, NULL, 0, RIL_PINSTATE_ENABLED_BLOCKED, RIL_PINSTATE_UNKNOWN },
        // SIM_NETWORK_PERSONALIZATION = 5
        { RIL_APPTYPE_SIM, RIL_APPSTATE_SUBSCRIPTION_PERSO, RIL_PERSOSUBSTATE_SIM_NETWORK,
          NULL, NULL, 0, RIL_PINSTATE_ENABLED_NOT_VERIFIED, RIL_PINSTATE_UNKNOWN }
    };
    RIL_CardState card_state;
    int num_apps;

    int sim_status = getSIMStatus();
    if (sim_status == SIM_ABSENT) {
        card_state = RIL_CARDSTATE_ABSENT;
        num_apps = 0;
    } else {
        card_state = RIL_CARDSTATE_PRESENT;
        num_apps = 1;
    }

    // Allocate and initialize base card status.
    RIL_CardStatus *p_card_status = malloc(sizeof(RIL_CardStatus));
    p_card_status->card_state = card_state;
    p_card_status->universal_pin_state = RIL_PINSTATE_UNKNOWN;
    p_card_status->gsm_umts_subscription_app_index = RIL_CARD_MAX_APPS;
    p_card_status->cdma_subscription_app_index = RIL_CARD_MAX_APPS;
    p_card_status->num_applications = num_apps;

    // Initialize application status
    int i;
    for (i = 0; i < RIL_CARD_MAX_APPS; i++) {
        p_card_status->applications[i] = app_status_array[SIM_ABSENT];
    }

    // Pickup the appropriate application status
    // that reflects sim_status for gsm.
    if (num_apps != 0) {
        // Only support one app, gsm
        p_card_status->num_applications = 1;
        p_card_status->gsm_umts_subscription_app_index = 0;

        // Get the correct app status
        p_card_status->applications[0] = app_status_array[sim_status];
    }

    *pp_card_status = p_card_status;
    return RIL_E_SUCCESS;
}

/**
 * Free the card status returned by getCardStatus
 */
static void freeCardStatus(RIL_CardStatus *p_card_status) {
    free(p_card_status);
}

/**
 * SIM ready means any commands that access the SIM will work, including:
 *  AT+CPIN, AT+CSMS, AT+CNMI, AT+CRSM
 *  (all SMS-related commands)
 */

static void pollSIMState (void *param)
{
    ATResponse *p_response;
    int ret;

    if (sState != RADIO_STATE_SIM_NOT_READY) {
        // no longer valid to poll
        return;
    }

    switch(getSIMStatus()) {
        case SIM_ABSENT:
        case SIM_PIN:
        case SIM_PUK:
        case SIM_NETWORK_PERSONALIZATION:
        default:
            setRadioState(RADIO_STATE_SIM_LOCKED_OR_ABSENT);
        return;

        case SIM_NOT_READY:
            RIL_requestTimedCallback (pollSIMState, NULL, &TIMEVAL_SIMPOLL);
        return;

        case SIM_READY:
            setRadioState(RADIO_STATE_SIM_READY);
        return;
    }
}

/** returns 1 if on, 0 if off, and -1 on error */
static int isRadioOn()
{
    ATResponse *p_response = NULL;
    int err;
    char *line;
    char ret;

    err = at_send_command_singleline("AT+CFUN?", "+CFUN:", &p_response);

    if (err < 0 || p_response->success == 0) {
        // assume radio is off
        goto error;
    }

    line = p_response->p_intermediates->line;

    err = at_tok_start(&line);
    if (err < 0) goto error;

    err = at_tok_nextbool(&line, &ret);
    if (err < 0) goto error;

    at_response_free(p_response);

    return (int)ret;

error:

    at_response_free(p_response);
    return -1;
}

/**
 * Initialize everything that can be configured while we're still in
 * AT+CFUN=0
 */
static void initializeCallback(void *param)
{
    ATResponse *p_response = NULL;
    int err;

    setRadioState (RADIO_STATE_OFF);
#if 0
    at_handshake();

    /* note: we don't check errors here. Everything important will
       be handled in onATTimeout and onATReaderClosed */
    
    /*  atchannel is tolerant of echo but it must */
    /*  have verbose result codes */
    at_send_command("ATE0Q0V1", NULL);

    /*  No auto-answer */
    at_send_command("ATS0=0", NULL);

    /*  Extended errors */
    at_send_command("AT+CMEE=1", NULL);

    /*  Network registration events */
    err = at_send_command("AT+CREG=2", &p_response);

    /* some handsets -- in tethered mode -- don't support CREG=2 */
    if (err < 0 || p_response->success == 0) {
        at_send_command("AT+CREG=1", NULL);
    }

    at_response_free(p_response);

    /*  GPRS registration events */
    at_send_command("AT+CGREG=1", NULL);

    /*  Call Waiting notifications */
    at_send_command("AT+CCWA=1", NULL);

    /*  Alternating voice/data off */
    at_send_command("AT+CMOD=0", NULL);

    /*  Not muted */
    at_send_command("AT+CMUT=0", NULL);

    /*  +CSSU unsolicited supp service notifications */
    at_send_command("AT+CSSN=0,1", NULL);

    /*  no connected line identification */
    at_send_command("AT+COLP=0", NULL);

    /*  HEX character set */
    at_send_command("AT+CSCS=\"HEX\"", NULL);

    /*  USSD unsolicited */
    at_send_command("AT+CUSD=1", NULL);

    /*  Enable +CGEV GPRS event notifications, but don't buffer */
    at_send_command("AT+CGEREP=1,0", NULL);

    /*  SMS PDU mode */
    at_send_command("AT+CMGF=0", NULL);

#ifdef USE_TI_COMMANDS

    at_send_command("AT%CPI=3", NULL);

    /*  TI specific -- notifications when SMS is ready (currently ignored) */
    at_send_command("AT%CSTAT=1", NULL);

#endif /* USE_TI_COMMANDS */


    /* assume radio is off on error */
    if (isRadioOn() > 0) {
        setRadioState (RADIO_STATE_SIM_NOT_READY);
    }
#endif
}

static void waitForClose()
{
    pthread_mutex_lock(&s_state_mutex);

    while (s_closed == 0) {
        pthread_cond_wait(&s_state_cond, &s_state_mutex);
    }

    pthread_mutex_unlock(&s_state_mutex);
}

/**
 * Called by atchannel when an unsolicited line appears
 * This is called on atchannel's reader thread. AT commands may
 * not be issued here
 */
static void onUnsolicited (const char *s, const char *sms_pdu)
{
    char *line = NULL;
    int err;

    /* Ignore unsolicited responses until we're initialized.
     * This is OK because the RIL library will poll for initial state
     */
    if (sState == RADIO_STATE_UNAVAILABLE) {
        return;
    }

    if (strStartsWith(s, "%CTZV:")
	|| strStartsWith(s,"+CTZV:")
	|| strStartsWith(s,"+CTZDST:")) {
      unsolicitedNitzTime(s);
    } else if (strStartsWith(s,"+CRING:")
                || strStartsWith(s,"RING")
                || strStartsWith(s,"NO CARRIER")
                || strStartsWith(s,"+CCWA")
    ) {
        RIL_onUnsolicitedResponse (
            RIL_UNSOL_RESPONSE_CALL_STATE_CHANGED,
            NULL, 0);
#ifdef WORKAROUND_FAKE_CGEV
        RIL_requestTimedCallback (onDataCallListChanged, NULL, NULL); //TODO use new function
#endif /* WORKAROUND_FAKE_CGEV */
    } else if (strStartsWith(s,"+XCIEV:")) {
        unsolicitedRSSI(s);
    } else if (strStartsWith(s,"+CREG:")
                || strStartsWith(s,"+CGREG:")
    ) {
        RIL_onUnsolicitedResponse (
            RIL_UNSOL_RESPONSE_NETWORK_STATE_CHANGED,
            NULL, 0);
#ifdef WORKAROUND_FAKE_CGEV
        RIL_requestTimedCallback (onDataCallListChanged, NULL, NULL);
#endif /* WORKAROUND_FAKE_CGEV */
    } else if (strStartsWith(s, "+CMT:")) {
        RIL_onUnsolicitedResponse (
            RIL_UNSOL_RESPONSE_NEW_SMS,
            sms_pdu, strlen(sms_pdu));
    } else if (strStartsWith(s, "+CDS:")) {
        RIL_onUnsolicitedResponse (
            RIL_UNSOL_RESPONSE_NEW_SMS_STATUS_REPORT,
            sms_pdu, strlen(sms_pdu));
    } else if (strStartsWith(s, "+CGEV:")) {
        /* Really, we can ignore NW CLASS and ME CLASS events here,
         * but right now we don't since extranous
         * RIL_UNSOL_DATA_CALL_LIST_CHANGED calls are tolerated
         */
        /* can't issue AT commands here -- call on main thread */
        RIL_requestTimedCallback (onDataCallListChanged, NULL, NULL);
#ifdef WORKAROUND_FAKE_CGEV
    } else if (strStartsWith(s, "+CME ERROR: 150")) {
        RIL_requestTimedCallback (onDataCallListChanged, NULL, NULL);
#endif /* WORKAROUND_FAKE_CGEV */
    }
}

/* Called on command or reader thread */
static void onATReaderClosed()
{
    LOGI("AT channel closed\n");
    at_close();
    s_closed = 1;

    setRadioState (RADIO_STATE_UNAVAILABLE);
}

/* Called on command thread */
static void onATTimeout()
{
    LOGI("AT channel timeout; closing\n");
    at_close();

    s_closed = 1;

    /* FIXME cause a radio reset here */

    setRadioState (RADIO_STATE_UNAVAILABLE);
}

static void usage(char *s)
{
#ifdef RIL_SHLIB
    fprintf(stderr, "reference-ril requires: -p <tcp port> or -d /dev/tty_device\n");
#else
    fprintf(stderr, "usage: %s [-p <tcp port>] [-d /dev/tty_device]\n", s);
    exit(-1);
#endif
}

static void *
mainLoop(void *param)
{
    int fd;
    int ret;

    AT_DUMP("== ", "entering mainLoop()", -1 );
    at_set_on_reader_closed(onATReaderClosed);
    at_set_on_timeout(onATTimeout);

    for (;;) {
        fd = -1;
        while  (fd < 0) {
            if (s_port > 0) {
                fd = socket_loopback_client(s_port, SOCK_STREAM);
            } else if (s_device_socket) {
                if (!strcmp(s_device_path, "/dev/socket/qemud")) {
                    /* Qemu-specific control socket */
                    fd = socket_local_client( "qemud",
                                              ANDROID_SOCKET_NAMESPACE_RESERVED,
                                              SOCK_STREAM );
                    if (fd >= 0 ) {
                        char  answer[2];

                        if ( write(fd, "gsm", 3) != 3 ||
                             read(fd, answer, 2) != 2 ||
                             memcmp(answer, "OK", 2) != 0)
                        {
                            close(fd);
                            fd = -1;
                        }
                   }
                }
                else
                    fd = socket_local_client( s_device_path,
                                            ANDROID_SOCKET_NAMESPACE_FILESYSTEM,
                                            SOCK_STREAM );
            } else if (s_device_path != NULL) {
                fd = open (s_device_path, O_RDWR);
                if ( fd >= 0 && !memcmp( s_device_path, "/dev/ttyS", 9 ) ) {
                    /* disable echo on serial ports */
                    struct termios  ios;
                    tcgetattr( fd, &ios );
                    ios.c_lflag = 0;  /* disable ECHO, ICANON, etc... */
                    tcsetattr( fd, TCSANOW, &ios );
                }
            }

            if (fd < 0) {
                perror ("opening AT interface. retrying...");
                sleep(10);
                /* never returns */
            }
        }

        s_closed = 0;
        ret = at_open(fd, onUnsolicited);

        if (ret < 0) {
            LOGE ("AT error %d on at_open\n", ret);
            return 0;
        }

        RIL_requestTimedCallback(initializeCallback, NULL, &TIMEVAL_0);

        // Give initializeCallback a chance to dispatched, since
        // we don't presently have a cancellation mechanism
        sleep(1);

        waitForClose();
        LOGI("Re-opening after close");
    }
}

static void import_kernel_nv(char *name)
{
    char setting[32];
    char *value = strchr(name, '=');

    if (value == 0) return;
    *value++ = 0;
    if (*name == 0) return;

#ifdef WORKAROUND_MODEM_AUDIO
    if (!strcmp(name,"ifxaudio")) {
        strlcpy(setting, value, sizeof(setting));
        ifxaudio = atoi(setting);

        LOGI_IF((ifxaudio == 1), "IFX Audio WorkWorkaround enabled\n");
    }
#endif
}

static void import_kernel_cmdline(void)
{
    char cmdline[1024];
    char *ptr;
    int fd;

    fd = open("/proc/cmdline", O_RDONLY);
    if (fd >= 0) {
        int n = read(fd, cmdline, 1023);
        if (n < 0) n = 0;

        /* get rid of trailing newline, it happens */
        if (n > 0 && cmdline[n-1] == '\n') n--;

        cmdline[n] = 0;

        close(fd);
    } else {
        cmdline[0] = 0;
    }

    ptr = cmdline;
    while (ptr && *ptr) {
        char *x = strchr(ptr, ' ');
        if (x != 0) *x++ = 0;
        import_kernel_nv(ptr);
        ptr = x;
    }
}

#ifdef RIL_SHLIB

pthread_t s_tid_mainloop;

const RIL_RadioFunctions *RIL_Init(const struct RIL_Env *env, int argc, char **argv)
{
    int ret;
    int fd = -1;
    int opt;
    pthread_attr_t attr;

    s_rilenv = env;

    while ( -1 != (opt = getopt(argc, argv, "p:d:s:"))) {
        switch (opt) {
            case 'p':
                s_port = atoi(optarg);
                if (s_port == 0) {
                    usage(argv[0]);
                    return NULL;
                }
                LOGI("Opening loopback port %d\n", s_port);
            break;

            case 'd':
                s_device_path = optarg;
                LOGI("Opening tty device %s\n", s_device_path);
            break;

            case 's':
                s_device_path   = optarg;
                s_device_socket = 1;
                LOGI("Opening socket %s\n", s_device_path);
            break;

            default:
                usage(argv[0]);
                return NULL;
        }
    }

    if (s_port < 0 && s_device_path == NULL) {
        usage(argv[0]);
        return NULL;
    }

#define PROPERTY_GPRS_CHANNEL	"vchanneld.gprs"
	char gprs_device[PROPERTY_VALUE_MAX];
	int fd_gprs;
    property_get(PROPERTY_GPRS_CHANNEL, gprs_device, "");
	LOGI("gprs device is %s", gprs_device);

	if (strlen(gprs_device) > 0)
		fd_gprs = open(gprs_device, O_RDWR | O_NONBLOCK);

	LOGI("fd_gprs is %d", fd_gprs);

    pthread_attr_init (&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    ret = pthread_create(&s_tid_mainloop, &attr, mainLoop, NULL);


//Initialize workaround used thanks to the bootargs
// bootargs supported are:
//     ifxaudio:   0 : disable code in workaround WORKAROUND_MODEM_AUDIO in RIL
//                 1 : enable code in workaround WORKAROUND_MODEM_AUDIO in RIL
//                 2 : disable code in workaround WORKAROUND_MODEM_AUDIO in RIL
//                     and Audio HAL can sent AT+XDRV command.
//                 by default it's set to 0 (ifxaudio=0).
    import_kernel_cmdline();

    return &s_callbacks;
}
#else /* RIL_SHLIB */
int main (int argc, char **argv)
{
    int ret;
    int fd = -1;
    int opt;

    while ( -1 != (opt = getopt(argc, argv, "p:d:"))) {
        switch (opt) {
            case 'p':
                s_port = atoi(optarg);
                if (s_port == 0) {
                    usage(argv[0]);
                }
                LOGI("Opening loopback port %d\n", s_port);
            break;

            case 'd':
                s_device_path = optarg;
                LOGI("Opening tty device %s\n", s_device_path);
            break;

            case 's':
                s_device_path   = optarg;
                s_device_socket = 1;
                LOGI("Opening socket %s\n", s_device_path);
            break;

            default:
                usage(argv[0]);
        }
    }

    if (s_port < 0 && s_device_path == NULL) {
        usage(argv[0]);
    }

    RIL_register(&s_callbacks);

    mainLoop(NULL);

    return 0;
}

#endif /* RIL_SHLIB */
