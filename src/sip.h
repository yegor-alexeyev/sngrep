/**************************************************************************
 **
 **  sngrep - Ncurses ngrep interface for SIP
 **
 **   Copyright (C) 2013 Ivan Alonso (Kaian)
 **   Copyright (C) 2013 Irontec SL. All rights reserved.
 **
 **   This program is free software: you can redistribute it and/or modify
 **   it under the terms of the GNU General Public License as published by
 **   the Free Software Foundation, either version 3 of the License, or
 **   (at your option) any later version.
 **
 **   This program is distributed in the hope that it will be useful,
 **   but WITHOUT ANY WARRANTY; without even the implied warranty of
 **   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 **   GNU General Public License for more details.
 **
 **   You should have received a copy of the GNU General Public License
 **   along with this program.  If not, see <http://www.gnu.org/licenses/>.
 **
 ****************************************************************************/
#ifndef __SNGREP_SIP_H
#define __SNGREP_SIP_H

#include <sys/time.h>

//! Shorter declaration of sip_call structure
typedef struct sip_call sip_call_t;
//! Shorter declaration of sip_msg structure
typedef struct sip_msg sip_msg_t;

/** 
 * This structure contains all required information of a single
 * message withing a dialog.
 *
 * Most of the data is just stored to be displayed in the UI so
 * the formats may be no the best, but the simplest for this
 * purpose. It also works as a linked lists of messages in a 
 * call.
 *
 */
struct sip_msg
{
    char date[9]; // FIXME for capturing at midnight?
    char time[18]; // FIXME this can be calculated
    char ip_from[22]; // Address including port
    char ip_to[22]; // Address including port
    char sip_from[256];
    char sip_to[256];
    char type[40];
	int cseq;
    time_t timet; // FIXME not required
    struct timeval ts;
    const char *payload[80]; // FIXME Payload in one struct
    int plines; // FIXME not required

    struct sip_call *call; /* Message owner */
    struct sip_msg *next; /* Messages linked list */
};

/**
 * This structure contains all required information of a call
 * and a pointer to the first message.
 *
 * Most of the data is just stored to be displayed in the UI so
 * the formats may be no the best, but the simplest for this
 * purpose. It also works as a linked lists of calls
 *
 */
struct sip_call
{
    char *callid; // Call-ID for this call
    char xcallid[800]; // FIXME Dynamic length
    int finished; // XXX NYI
    /* pthread_mutex_t */// XXX NYI

    struct sip_msg *messages; /* Messages of this call */
    struct sip_call *next; /* Calls double linked list */
    struct sip_call *prev; /* Calls double linked list */
};

/**
 * This function parses ngrep header and SIP message payload to 
 * fill a sip_message structure.
 *
 * If no call is found with the given Call-ID, a new one will be
 * created and added to calls list.
 *
 * @param header ngrep header generated by -qpt arguments
 * @param payload SIP message payload
 * @returns the message structure @sip_msg or NULL if parsed failed
 *
 */
struct sip_msg *sip_parse_message(const char *header, const char *payload);

/**
 * Parses Call-ID header of a SIP message payload
 * 
 * @param payload SIP message payload
 * @returns callid parsed from Call-ID header 
 * @note the returned pointer MUST be deallocated after use
 */
char *get_callid(const char* payload);

/**
 * Create a new call with the given callid (Minimum required data)
 *
 * @param callid Call-ID Header value 
 * @returns pointer to the sip_call created
 */
struct sip_call *call_new(const char *callid);

/**
 * Parse the ngrep header and payload and add the result message
 * to the given call.
 *
 * @param call pointer to the call owner of the message
 * @param header ngrep header generated by -qpt arguments
 * @param payload SIP message payload
 * @retursn the message structure or NULL if parsed failed
 */
struct sip_msg *call_add_message(struct sip_call *call, const char *header, const char *payload);

/**
 * Parse ngrep header line to get timestamps and ip addresses
 * of the SIP message.
 *
 * @param msg SIP message structure 
 * @param header ngrep header generated by -qpt arguments
 * @returns 0 on success, 1 on malformed header
 */
int msg_parse_header(struct sip_msg *msg, const char *header);

/**
 * Parse SIP Message payload to fill sip_msg structe
 * 
 * @param msg SIP message structure
 * @param payload SIP message payload
 * @returns 0 in all cases 
 */
int msg_parse_payload(struct sip_msg *msg, const char *payload);

/**
 * Find a call structure in calls linked list given an xcallid
 *
 * @param xcallid X-Call-ID or X-CID Header value
 * @returns pointer to the sip_call structure found or NULL 
 */
struct sip_call *call_find_by_xcallid(const char *xcallid);

/**
 * Find a call structure in calls linked list given an callid
 *
 * @param callid Call-ID Header value
 * @returns pointer to the sip_call structure found or NULL 
 */
struct sip_call *call_find_by_callid(const char *callid);

/**
 * Getter for calls linked list size
 *
 * @returns how many calls are linked in the list 
 */
int get_n_calls();

/**
 * Getter for call messages linked list size
 *
 * @returns how many messages are in the call
 */
int get_n_msgs(const struct sip_call *call);

/**
 * Finds the other leg of this call.
 *
 * If this call has a X-CID or X-Call-ID header, that call will be
 * find and returned. Otherwise, a call with X-CID or X-Call-ID header 
 * matching the given call's Call-ID will be find or returned.
 *
 * @param call SIP call structure
 * @returns The other call structure or NULL if none found
 */
struct sip_call *get_ex_call(const struct sip_call *call);

/**
 * Finds the next msg in a call. If the passed msg is
 * NULL it returns the first message in the call 
 *
 * @param msg Actual SIP msg from the call (can be NULL)
 * @returns Next chronological message in the call
 */
struct sip_msg *get_next_msg(const struct sip_call *call, const struct sip_msg *msg);

/**
 * Finds the next msg in call and it's extended. If the passed msg is
 * NULL it returns the first message in the conversation
 *
 * @param call SIP call structure
 * @param msg Actual SIP msg from the call (can be NULL)
 * @returns Next chronological message in the conversation
 *
 */
struct sip_msg *get_next_msg_ex(const struct sip_call *call, const struct sip_msg *msg);

#endif