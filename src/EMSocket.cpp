//
// This file is part of the aMule Project.
//
// Copyright (c) 2003-2005 aMule Team ( admin@amule.org / http://www.amule.org )
// Copyright (c) 2002 Merkur ( devs@emule-project.net / http://www.emule-project.net )
//
// Any parts of this program derived from the xMule, lMule or eMule project,
// or contributed by third-party developers are copyrighted by their
// respective authors.
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
// 
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA, 02111-1307, USA
//

#if defined(__GNUG__) && !defined(NO_GCC_PRAGMA)
#pragma implementation "EMSocket.h"
#endif

#include <csignal>
#include <fcntl.h>
#include <sys/stat.h>

#include "EMSocket.h"		// Interface declarations.
#include "Packet.h"		// Needed for CPacket
#include "amule.h"
#include "GetTickCount.h"
#include "UploadBandwidthThrottler.h"
#include "Logger.h"

const uint32 MAX_SIZE = 2000000;

IMPLEMENT_DYNAMIC_CLASS(CEMSocket,wxSocketClient)

CEMSocket::CEMSocket(const CProxyData *ProxyData)
	: CSocketClientProxy(wxSOCKET_NOWAIT, ProxyData)
{
	byConnected = ES_NOTCONNECTED;
	m_uTimeOut = CONNECTION_TIMEOUT; // default timeout for ed2k sockets

	// Download (pseudo) rate control	
	downloadLimit = 0;
	downloadLimitEnable = false;
	pendingOnReceive = false;

	// Download partial header
	pendingHeaderSize = 0;

	// Download partial packet
	pendingPacket = NULL;
	pendingPacketSize = 0;

	// Upload control
	sendbuffer = NULL;
	sendblen = 0;
	sent = 0;

    m_currentPacket_is_controlpacket = false;
	m_currentPackageIsFromPartFile = false;

    m_numberOfSentBytesCompleteFile = 0;
    m_numberOfSentBytesPartFile = 0;
    m_numberOfSentBytesControlPacket = 0;

    lastCalledSend = ::GetTickCount();
    lastSent = ::GetTickCount()-1000;

	m_bAccelerateUpload = false;

    m_actualPayloadSize = 0;
    m_actualPayloadSizeSent = 0;

    m_bBusy = false;
    m_hasSent = false;

	lastFinishedStandard = 0;

	DoingDestroy = false;
	
}

CEMSocket::~CEMSocket()
{
    // need to be locked here to know that the other methods
    // won't be in the middle of things
	m_sendLocker.Lock();
	byConnected = ES_DISCONNECTED;
	m_sendLocker.Unlock();

    // now that we know no other method will keep adding to the queue
    // we can remove ourself from the queue
	if (theApp.uploadBandwidthThrottler) {
	    theApp.uploadBandwidthThrottler->RemoveFromAllQueues(this);
	}

    ClearQueues();
	
	SetNotify(0);
	Notify(FALSE);
}

void CEMSocket::Destroy() {
	if (!DoingDestroy) {		
		DoingDestroy = true;
		wxSocketClient::Destroy();
	}	
}


void CEMSocket::ClearQueues()
{
	for (POSITION pos = controlpacket_queue.GetHeadPosition();pos != 0;) {
		delete controlpacket_queue.GetNext(pos);
	}
	controlpacket_queue.RemoveAll();
	for (POSITION pos = standartpacket_queue.GetHeadPosition();pos != 0;) {
		delete standartpacket_queue.GetNext(pos).packet;
	}
	standartpacket_queue.RemoveAll();
	
	// Download (pseudo) rate control	
	downloadLimit = 0;
	downloadLimitEnable = false;
	pendingOnReceive = false;

	// Download partial header
	pendingHeaderSize = 0;

	// Download partial packet
	delete pendingPacket;
	pendingPacket = NULL;
	pendingPacketSize = 0;

	// Upload control
	delete[] sendbuffer;
	sendbuffer = NULL;
	sendblen = 0;
	sent = 0;
}


void CEMSocket::OnClose(int WXUNUSED(nErrorCode))
{
    // need to be locked here to know that the other methods
    // won't be in the middle of things
	m_sendLocker.Lock();
	byConnected = ES_DISCONNECTED;
	m_sendLocker.Unlock();

    // now that we know no other method will keep adding to the queue
    // we can remove ourself from the queue
    theApp.uploadBandwidthThrottler->RemoveFromAllQueues(this);
	
	ClearQueues();
}


void CEMSocket::OnReceive(int nErrorCode)
{
	// the 2 meg size was taken from another place
	static char GlobalReadBuffer[MAX_SIZE];

	if(nErrorCode) {
		uint32 error = LastError(); 
		if (error != wxSOCKET_WOULDBLOCK) {
			OnError(nErrorCode);
			return;
		}
	}
	
	// Check current connection state
	if(byConnected == ES_DISCONNECTED) {
		return;
	} else {	
		byConnected = ES_CONNECTED; // ES_DISCONNECTED, ES_NOTCONNECTED, ES_CONNECTED
	}

	// CPU load improvement
	if(downloadLimitEnable == true && downloadLimit == 0){
		pendingOnReceive = true;
		return;
	}

	// Remark: an overflow can not occur here
	uint32 readMax = sizeof(GlobalReadBuffer) - pendingHeaderSize; 
	if((downloadLimitEnable == true) && (readMax > downloadLimit)) {
		readMax = downloadLimit;
	}

	// We attempt to read up to 2 megs at a time (minus whatever is in our internal read buffer)
	Read(GlobalReadBuffer + pendingHeaderSize, readMax);
	uint32 ret=LastCount();
	if (Error() || ret == 0) {
		if (LastError() == wxSOCKET_WOULDBLOCK) {
			pendingOnReceive = true;
		}
		return;
	}
	
	
	// Bandwidth control
	if(downloadLimitEnable == true){
		// Update limit
		downloadLimit -= ret;
	}

	// CPU load improvement
	// Detect if the socket's buffer is empty (or the size did match...)
	pendingOnReceive = (ret == readMax);

	// Copy back the partial header into the global read buffer for processing
	if(pendingHeaderSize > 0) {
  		memcpy(GlobalReadBuffer, pendingHeader, pendingHeaderSize);
		ret += pendingHeaderSize;
		pendingHeaderSize = 0;
	}

	char *rptr = GlobalReadBuffer; // floating index initialized with begin of buffer
	const char *rend = GlobalReadBuffer + ret; // end of buffer

	// Loop, processing packets until we run out of them
	while((rend - rptr >= PACKET_HEADER_SIZE) ||
	      ((pendingPacket != NULL) && (rend - rptr > 0 ))){ 

		// Two possibilities here: 
		//
		// 1. There is no pending incoming packet
		// 2. There is already a partial pending incoming packet
		//
		// It's important to remember that emule exchange two kinds of packet
		// - The control packet
		// - The data packet for the transport of the block
		// 
		// The biggest part of the traffic is done with the data packets. 
		// The default size of one block is 10240 bytes (or less if compressed), but the
		// maximal size for one packet on the network is 1300 bytes. It's the reason
		// why most of the Blocks are splitted before to be sent. 
		//
		// Conclusion: When the download limit is disabled, this method can be at least 
		// called 8 times (10240/1300) by the lower layer before a splitted packet is 
		// rebuild and transfered to the above layer for processing.
		//
		// The purpose of this algorithm is to limit the amount of data exchanged between buffers

		if(pendingPacket == NULL){
			pendingPacket = new CPacket(rptr); // Create new packet container. 
			rptr += 6;                        // Only the header is initialized so far

			// Bugfix We still need to check for a valid protocol
			// Remark: the default eMule v0.26b had removed this test......
			switch (pendingPacket->GetProtocol()){
				case OP_EDONKEYPROT:
				case OP_PACKEDPROT:
				case OP_EMULEPROT:
					break;
				default:
					delete pendingPacket;
					pendingPacket = NULL;
					OnError(ERR_WRONGHEADER);
					return;
			}

			// Security: Check for buffer overflow (2MB)
			if(pendingPacket->GetPacketSize() > sizeof(GlobalReadBuffer)) {
				delete pendingPacket;
				pendingPacket = NULL;
				OnError(ERR_TOOBIG);
				return;
			}

			// Init data buffer
			pendingPacket->AllocDataBuffer();
			pendingPacketSize = 0;
		}

		// Bytes ready to be copied into packet's internal buffer
		wxASSERT(rptr <= rend);
		uint32 toCopy = ((pendingPacket->GetPacketSize() - pendingPacketSize) < (uint32)(rend - rptr)) ? 
					(pendingPacket->GetPacketSize() - pendingPacketSize) : (uint32)(rend - rptr);

		// Copy Bytes from Global buffer to packet's internal buffer
		pendingPacket->CopyToDataBuffer(pendingPacketSize, rptr, toCopy);
		pendingPacketSize += toCopy;
		rptr += toCopy;
		
		// Check if packet is complet
		wxASSERT(pendingPacket->GetPacketSize() >= pendingPacketSize);
		if(pendingPacket->GetPacketSize() == pendingPacketSize) {
			// Process packet
			bool bPacketResult = PacketReceived(pendingPacket);
			delete pendingPacket;	
			pendingPacket = NULL;
			pendingPacketSize = 0;
			
			if (!bPacketResult)
				return;			
		}
	}

	// Finally, if there is any data left over, save it for next time
	wxASSERT(rptr <= rend);
	wxASSERT(rend - rptr < PACKET_HEADER_SIZE);
	if(rptr != rend) {
		// Keep the partial head
		pendingHeaderSize = rend - rptr;
		memcpy(pendingHeader, rptr, pendingHeaderSize);
	}	
}


void CEMSocket::SetDownloadLimit(uint32 limit)
{
	downloadLimit = limit;
	downloadLimitEnable = true;	
	
	// CPU load improvement
	if(limit > 0 && pendingOnReceive == true){
		OnReceive(0);
	}
}


void CEMSocket::DisableDownloadLimit()
{
	downloadLimitEnable = false;

	// CPU load improvement
	if (pendingOnReceive == true){
		OnReceive(0);
	}
}


/**
 * Queues up the packet to be sent. Another thread will actually send the packet.
 *
 * If the packet is not a control packet, and if the socket decides that its queue is
 * full and forceAdd is false, then the socket is allowed to refuse to add the packet
 * to its queue. It will then return false and it is up to the calling thread to try
 * to call SendPacket for that packet again at a later time.
 *
 * @param packet address to the packet that should be added to the queue
 *
 * @param delpacket if true, the responsibility for deleting the packet after it has been sent
 *                  has been transferred to this object. If false, don't delete the packet after it
 *                  has been sent.
 *
 * @param controlpacket the packet is a controlpacket
 *
 * @param forceAdd this packet must be added to the queue, even if it is full. If this flag is true
 *                 then the method can not refuse to add the packet, and therefore not return false.
 *
 * @return true if the packet was added to the queue, false otherwise
 */
void CEMSocket::SendPacket(CPacket* packet, bool delpacket, bool controlpacket, uint32 actualPayloadSize)
{
	wxMutexLocker lock(m_sendLocker);

	if (byConnected == ES_DISCONNECTED) {
        if(delpacket) {
			delete packet;
        }
    } else {
        if (!delpacket){
            packet = new CPacket(*packet);
	    }

        if (controlpacket) {
	        controlpacket_queue.AddTail(packet);

            // queue up for controlpacket
            theApp.uploadBandwidthThrottler->QueueForSendingControlPacket(this, HasSent());
	    } else {
            bool first = !((sendbuffer && !m_currentPacket_is_controlpacket) || !standartpacket_queue.IsEmpty());
            StandardPacketQueueEntry queueEntry = { actualPayloadSize, packet };
		    standartpacket_queue.AddTail(queueEntry);

            // reset timeout for the first time
            if (first) {
                lastFinishedStandard = ::GetTickCount();
                m_bAccelerateUpload = true;	// Always accelerate first packet in a block
            }
	    }
    }
}


uint64 CEMSocket::GetSentBytesCompleteFileSinceLastCallAndReset()
{
	wxMutexLocker lock( m_sendLocker );

	uint64 sentBytes = m_numberOfSentBytesCompleteFile;
    m_numberOfSentBytesCompleteFile = 0;

    return sentBytes;
}


uint64 CEMSocket::GetSentBytesPartFileSinceLastCallAndReset()
{
	wxMutexLocker lock( m_sendLocker );

	uint64 sentBytes = m_numberOfSentBytesPartFile;
    m_numberOfSentBytesPartFile = 0;

    return sentBytes;
}

uint64 CEMSocket::GetSentBytesControlPacketSinceLastCallAndReset()
{
	wxMutexLocker lock( m_sendLocker );

	uint64 sentBytes = m_numberOfSentBytesControlPacket;
    m_numberOfSentBytesControlPacket = 0;

    return sentBytes;
}

uint64 CEMSocket::GetSentPayloadSinceLastCallAndReset()
{
	wxMutexLocker lock( m_sendLocker );

	uint64 sentBytes = m_actualPayloadSizeSent;
    m_actualPayloadSizeSent = 0;

    return sentBytes;
}


void CEMSocket::OnSend(int nErrorCode)
{
    if (nErrorCode){
		OnError(nErrorCode);
		return;
	}

	wxMutexLocker lock( m_sendLocker );
    m_bBusy = false;

    if (byConnected != ES_DISCONNECTED) {
		byConnected = ES_CONNECTED;

	    if (m_currentPacket_is_controlpacket) {
	        // queue up for control packet
    	    theApp.uploadBandwidthThrottler->QueueForSendingControlPacket(this, HasSent());
		}
    }
}


/**
 * Try to put queued up data on the socket.
 *
 * Control packets have higher priority, and will be sent first, if possible.
 * Standard packets can be split up in several package containers. In that case
 * all the parts of a split package must be sent in a row, without any control packet
 * in between.
 *
 * @param maxNumberOfBytesToSend This is the maximum number of bytes that is allowed to be put on the socket
 *                               this call. The actual number of sent bytes will be returned from the method.
 *
 * @param onlyAllowedToSendControlPacket This call we only try to put control packets on the sockets.
 *                                       If there's a standard packet "in the way", and we think that this socket
 *                                       is no longer an upload slot, then it is ok to send the standard packet to
 *                                       get it out of the way. But it is not allowed to pick a new standard packet
 *                                       from the queue during this call. Several split packets are counted as one
 *                                       standard packet though, so it is ok to finish them all off if necessary.
 *
 * @return the actual number of bytes that were put on the socket.
 */
SocketSentBytes CEMSocket::Send(uint32 maxNumberOfBytesToSend, uint32 minFragSize, bool onlyAllowedToSendControlPacket)
{
	wxMutexLocker lock(m_sendLocker);

	if (byConnected == ES_DISCONNECTED) {
        SocketSentBytes returnVal = { false, 0, 0 };
        return returnVal;
    } else if (m_bBusy && onlyAllowedToSendControlPacket) {
        SocketSentBytes returnVal = { true, 0, 0 };
        return returnVal;
    }

    if(minFragSize < 1) {
        minFragSize = 1;
    }

    maxNumberOfBytesToSend = GetNextFragSize(maxNumberOfBytesToSend, minFragSize);

    bool bWasLongTimeSinceSend = (::GetTickCount() - lastSent) > 1000;

    lastCalledSend = ::GetTickCount();

    bool anErrorHasOccured = false;
    uint32 sentStandardPacketBytesThisCall = 0;
    uint32 sentControlPacketBytesThisCall = 0;
	
    while(sentStandardPacketBytesThisCall + sentControlPacketBytesThisCall < maxNumberOfBytesToSend && anErrorHasOccured == false && // don't send more than allowed. Also, there should have been no error in earlier loop
          (!controlpacket_queue.IsEmpty() || !standartpacket_queue.IsEmpty() || sendbuffer != NULL) && // there must exist something to send
          (onlyAllowedToSendControlPacket == false || // this means we are allowed to send both types of packets, so proceed
           sentStandardPacketBytesThisCall + sentControlPacketBytesThisCall > 0 && (sentStandardPacketBytesThisCall + sentControlPacketBytesThisCall) % minFragSize != 0 ||
           sendbuffer == NULL && !controlpacket_queue.IsEmpty() || // There's a control packet in queue, and we are not currently sending anything, so we will handle the control packet next
           sendbuffer != NULL && m_currentPacket_is_controlpacket == true || // We are in the progress of sending a control packet. We are always allowed to send those
           sendbuffer != NULL && m_currentPacket_is_controlpacket == false && bWasLongTimeSinceSend && !controlpacket_queue.IsEmpty() && standartpacket_queue.IsEmpty() && (sentStandardPacketBytesThisCall + sentControlPacketBytesThisCall) < minFragSize // We have waited to long to clean the current packet (which may be a standard packet that is in the way). Proceed no matter what the value of onlyAllowedToSendControlPacket.
          )
         ) {

        // If we are currently not in the progress of sending a packet, we will need to find the next one to send
        if(sendbuffer == NULL) {
            CPacket* curPacket = NULL;
            if(!controlpacket_queue.IsEmpty()) {
                // There's a control packet to send
                m_currentPacket_is_controlpacket = true;
                curPacket = controlpacket_queue.RemoveHead();
            } else if(!standartpacket_queue.IsEmpty() /*&& onlyAllowedToSendControlPacket == false*/) {
                // There's a standard packet to send
                m_currentPacket_is_controlpacket = false;
                StandardPacketQueueEntry queueEntry = standartpacket_queue.RemoveHead();
                curPacket = queueEntry.packet;
                m_actualPayloadSize = queueEntry.actualPayloadSize;

                // remember this for statistics purposes.
                m_currentPackageIsFromPartFile = curPacket->IsFromPF();
            } else {
                // Just to be safe. Shouldn't happen?
                // if we reach this point, then there's something wrong with the while condition above!
                wxASSERT(0);
                AddDebugLogLineM(true, logGeneral, wxT("EMSocket: Couldn't get a new packet! There's an error in the first while condition in EMSocket::Send()"));

                SocketSentBytes returnVal = { true, sentStandardPacketBytesThisCall, sentControlPacketBytesThisCall };
                return returnVal;
            }

            // We found a package to send. Get the data to send from the
            // package container and dispose of the container.
            sendblen = curPacket->GetRealPacketSize();
            sendbuffer = curPacket->DetachPacket();
            sent = 0;
            delete curPacket;
        }

        // At this point we've got a packet to send in sendbuffer. Try to send it. Loop until entire packet
        // is sent, or until we reach maximum bytes to send for this call, or until we get an error.
        // NOTE! If send would block (returns WSAEWOULDBLOCK), we will return from this method INSIDE this loop.
        while (sent < sendblen &&
               sentStandardPacketBytesThisCall + sentControlPacketBytesThisCall < maxNumberOfBytesToSend &&
               (
                onlyAllowedToSendControlPacket == false || // this means we are allowed to send both types of packets, so proceed
                m_currentPacket_is_controlpacket ||
                bWasLongTimeSinceSend && (sentStandardPacketBytesThisCall + sentControlPacketBytesThisCall) < minFragSize ||
                (sentStandardPacketBytesThisCall + sentControlPacketBytesThisCall) % minFragSize != 0
               ) &&
               anErrorHasOccured == false) {
		    uint32 tosend = sendblen-sent;
            if(!onlyAllowedToSendControlPacket || m_currentPacket_is_controlpacket) {
    		    if (maxNumberOfBytesToSend >= sentStandardPacketBytesThisCall + sentControlPacketBytesThisCall && tosend > maxNumberOfBytesToSend-(sentStandardPacketBytesThisCall + sentControlPacketBytesThisCall))
                    tosend = maxNumberOfBytesToSend-(sentStandardPacketBytesThisCall + sentControlPacketBytesThisCall);
            } else if(bWasLongTimeSinceSend && (sentStandardPacketBytesThisCall + sentControlPacketBytesThisCall) < minFragSize) {
    		    if (minFragSize >= sentStandardPacketBytesThisCall + sentControlPacketBytesThisCall && tosend > minFragSize-(sentStandardPacketBytesThisCall + sentControlPacketBytesThisCall))
                    tosend = minFragSize-(sentStandardPacketBytesThisCall + sentControlPacketBytesThisCall);
            } else {
                uint32 nextFragMaxBytesToSent = GetNextFragSize(sentStandardPacketBytesThisCall + sentControlPacketBytesThisCall, minFragSize);
    		    if (nextFragMaxBytesToSent >= sentStandardPacketBytesThisCall + sentControlPacketBytesThisCall && tosend > nextFragMaxBytesToSent-(sentStandardPacketBytesThisCall + sentControlPacketBytesThisCall))
                    tosend = nextFragMaxBytesToSent-(sentStandardPacketBytesThisCall + sentControlPacketBytesThisCall);
            }
		    wxASSERT(tosend != 0 && tosend <= sendblen-sent);
    		
            //DWORD tempStartSendTick = ::GetTickCount();

            lastSent = ::GetTickCount();

			wxSocketBase::Write(sendbuffer+sent,tosend);
			uint32 result = LastCount();
		
		    if (Error()){
				if (result)
					printf("LastCount: %u\n", result);
				
			    uint32 error = LastError();
			    if (error == wxSOCKET_WOULDBLOCK){
                    m_bBusy = true;

                    SocketSentBytes returnVal = { true, sentStandardPacketBytesThisCall, sentControlPacketBytesThisCall };
                    
					return returnVal; // Send() blocked, onsend will be called when ready to send again
			    } else{
                    // Send() gave an error
                    anErrorHasOccured = true;
                }
            } else {
                // we managed to send some bytes. Perform bookkeeping.
                m_bBusy = false;
                m_hasSent = true;

                sent += result;

                // Log send bytes in correct class
                if(m_currentPacket_is_controlpacket == false) {
                    sentStandardPacketBytesThisCall += result;

                    if(m_currentPackageIsFromPartFile == true) {
                        m_numberOfSentBytesPartFile += result;
                    } else {
                        m_numberOfSentBytesCompleteFile += result;
                    }
                } else {
                    sentControlPacketBytesThisCall += result;
                    m_numberOfSentBytesControlPacket += result;
                }
            }
	    }

        if (sent == sendblen){
            // we are done sending the current package. Delete it and set
            // sendbuffer to NULL so a new packet can be fetched.
		    delete[] sendbuffer;
		    sendbuffer = NULL;
			sendblen = 0;

            if(!m_currentPacket_is_controlpacket) {
                m_actualPayloadSizeSent += m_actualPayloadSize;
                m_actualPayloadSize = 0;

                lastFinishedStandard = ::GetTickCount(); // reset timeout
                m_bAccelerateUpload = false; // Safe until told otherwise
            }

            sent = 0;
        }
    }


    if(onlyAllowedToSendControlPacket && (!controlpacket_queue.IsEmpty() || sendbuffer != NULL && m_currentPacket_is_controlpacket)) {
        // enter control packet send queue
        // we might enter control packet queue several times for the same package,
        // but that costs very little overhead. Less overhead than trying to make sure
        // that we only enter the queue once.
        theApp.uploadBandwidthThrottler->QueueForSendingControlPacket(this, HasSent());
    }

    SocketSentBytes returnVal = { !anErrorHasOccured, sentStandardPacketBytesThisCall, sentControlPacketBytesThisCall };

    return returnVal;
}


uint32 CEMSocket::GetNextFragSize(uint32 current, uint32 minFragSize)
{
    if(current % minFragSize == 0) {
        return current;
    } else {
        return minFragSize*(current/minFragSize+1);
    }
}


/**
 * Decides the (minimum) amount the socket needs to send to prevent timeout.
 * 
 * @author SlugFiller
 */
uint32 CEMSocket::GetNeededBytes()
{
	m_sendLocker.Lock();
	if (byConnected == ES_DISCONNECTED) {
		m_sendLocker.Unlock();
		return 0;
	}

    if (!((sendbuffer && !m_currentPacket_is_controlpacket) || !standartpacket_queue.IsEmpty())) {
    	// No standard packet to send. Even if data needs to be sent to prevent timout, there's nothing to send.
		m_sendLocker.Unlock();
		return 0;
	}

	if (((sendbuffer && !m_currentPacket_is_controlpacket)) && !controlpacket_queue.IsEmpty())
		m_bAccelerateUpload = true;	// We might be trying to send a block request, accelerate packet

	uint32 sendgap = ::GetTickCount() - lastCalledSend;

	uint64 timetotal = m_bAccelerateUpload?45000:90000;
	uint64 timeleft = ::GetTickCount() - lastFinishedStandard;
	uint64 sizeleft, sizetotal;
	if (sendbuffer && !m_currentPacket_is_controlpacket) {
		sizeleft = sendblen-sent;
		sizetotal = sendblen;
	} else {
		sizeleft = sizetotal = standartpacket_queue.GetHead().packet->GetRealPacketSize();
	}
	m_sendLocker.Unlock();

	if (timeleft >= timetotal)
		return sizeleft;
	timeleft = timetotal-timeleft;
	if (timeleft*sizetotal >= timetotal*sizeleft) {
		// don't use 'GetTimeOut' here in case the timeout value is high,
		if (sendgap > SEC2MS(20))
			return 1;	// Don't let the socket itself time out - Might happen when switching from spread(non-focus) slot to trickle slot
		return 0;
	}
	uint64 decval = timeleft*sizetotal/timetotal;
	if (!decval)
		return sizeleft;
	if (decval < sizeleft)
		return sizeleft-decval+1;	// Round up
	else
		return 1;
}


/**
 * Removes all packets from the standard queue that don't have to be sent for the socket to be able to send a control packet.
 *
 * Before a socket can send a new packet, the current packet has to be finished. If the current packet is part of
 * a split packet, then all parts of that split packet must be sent before the socket can send a control packet.
 *
 * This method keeps in standard queue only those packets that must be sent (rest of split packet), and removes everything
 * after it. The method doesn't touch the control packet queue.
 */
void CEMSocket::TruncateQueues()
{
	wxMutexLocker lock(m_sendLocker);

	// Clear the standard queue totally
    // Please note! There may still be a standardpacket in the sendbuffer variable!
	for(POSITION pos = standartpacket_queue.GetHeadPosition(); pos != NULL; )
		delete standartpacket_queue.GetNext(pos).packet;
	standartpacket_queue.RemoveAll();
}


uint32 CEMSocket::GetTimeOut() const
{
	return m_uTimeOut;
}


void CEMSocket::SetTimeOut(uint32 uTimeOut)
{
	m_uTimeOut = uTimeOut;
}

