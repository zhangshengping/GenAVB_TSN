/*
 * Copyright 2014-2016 Freescale Semiconductor, Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *    Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *    Neither the name of NXP Semiconductors nor the
 *    names of its contributors may be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL <COPYRIGHT HOLDER> BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/**
 \file rtp.h
 \brief GenAVB public API
 \details RTP header definitions.

 \copyright Copyright 2015 Freescale Semiconductor, Inc.
*/
#ifndef _GENAVB_PUBLIC_RTP_H_
#define _GENAVB_PUBLIC_RTP_H_

#include "types.h"

#define L5PROTO_RTP	1
#define L5PROTO_RTCP	2
#define L5PROTO_ANY	0xff

/* see RFC3551 table 4 - https://tools.ietf.org/html/rfc3551 */
#define RTP_PT_PCMU 		0 /* PCM u-law audio 64 kbit/s */
#define RTP_PT_PCMA 		8 /* PCM A-law audio 64 kbit/s */
#define RTP_PT_L16_2 		10 /* customized (48kHz) L16/2-channels */
#define RTP_PT_L16 			11 /* customized (48kHz) L16/1-channel */
#define RTP_PT_DYNAMIC		96 /* Dynamic */

struct rtp_hdr {
#if defined(__BIG_ENDIAN__)
	avb_u8	version:2,	/* protocol version */
		p:1,		/* padding flag */
		x:1,		/* header extension flag */
		cc:4;		/* CSRC count */

	avb_u8	m:1,		/* marker bit */
		pt:7;		/* payload type */
#else
	avb_u8	cc:4,
		x:1,
		p:1,
		version:2;

	avb_u8	pt:7,
		m:1;
#endif

	avb_u16	seq;		/* sequence number */
	avb_u32	ts;		/* timestamp */
	avb_u32	ssrc;		/* synchronization source */
};


#endif /* _GENAVB_PUBLIC_RTP_H_*/
