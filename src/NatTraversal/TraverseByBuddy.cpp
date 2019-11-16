#include "stdafx.h"
#include "TraverseByBuddy.h"

#include "../emule.h"
#include "NatThread.h"
#include "NatSocket.h"
#include "ClientList.h"
#include "updownclient.h"
#include "ListenSocket.h"
#include "sockets.h"
#include "kademlia\kademlia\kademlia.h"

CTraverseByBuddy::CTraverseByBuddy(const uchar * uh, CTraverseStrategy * pNext)  : CTraverseStrategy(uh, pNext)
{
	m_dwTraverseIp = 0;
	m_wTraversePort = 0;

	m_Sock = NULL;

	m_SendPingTime = m_SendReqTime = m_SendConnAckTime = 0;
	m_nSendReqCount = 0;
	m_nSendPingCount = 0;
	m_nPassivePing = 0;

	m_bAcceptor = false;
	m_dwClientIp = 0;
	m_wClientPort = 0;

	m_dwState = 0;
	m_nSendConnAck = 0;
	m_dwConnAskNumber = GetTickCount();

}

void CTraverseByBuddy::SendPacket()
{
	try
	{
		if(!m_bFailed && (m_dwState&NAT_S_SYNC)==0 && time(NULL)-m_SendReqTime > 10)
		{
			//  failed to connect
			if(m_nSendReqCount>60)
			{
				m_dwState|=NAT_E_TIMEOUT;
				if(m_nSendReqCount>70)
					Failed();
				m_nSendReqCount++;
			}
			else
			{
				SendConnectReq();
				SendPingPacket();
			}
		}

		if(m_bFailed)
		{
			if(m_Sock)// && pClientSock->client)
			{
				//m_Sock->m_bUseNat = false;
			}
			_AddLogLine(false, _T("Unconnected NatSock was deleted. time out. %s."), UserHashToString(m_UserHash));
		}
	}
	catch(...)
	{
		//  the CAsyncSocketEx maybe is deleted
		TRACE("Exception: %s\n", __FUNCTION__);
		Failed();
	}
}

int CTraverseByBuddy::SendConnectReq()
{
	if(m_dwState & NAT_E_TIMEOUT) return 1;

	m_SendReqTime = time(NULL);
	m_nSendReqCount++;

	CClientReqSocket * pClientSock = NULL;
	try
	{
		pClientSock=DYNAMIC_DOWNCAST(CClientReqSocket, m_Sock);
	}
	catch(...)
	{
		TRACE("Exception: %s\n", __FUNCTION__);
		Failed();
		return 1;
	}

	if(pClientSock && pClientSock->client)
	{
		m_dwTraverseIp = pClientSock->client->GetBuddyIP();
		m_wTraversePort = htons(pClientSock->client->GetBuddyPort());

		//if(!m_dwTraverseIp || !m_wTraversePort)
		//	return 1;
	}
	else
	{
		//T_TRACE("failed to get buddy addr by CUpdownClient");
		Failed();
		return 1;
	}

	Kademlia::CUInt128 uBuddyID(true);
	uBuddyID.Xor(pClientSock->client->GetBuddyID());
	byte cID[16];
	uBuddyID.ToByteArray(cID);

	const uchar *uh=m_UserHash;
	TRACE(_T("send sync to connect %02X%02X%02X%02X by buddy. kad id=%s\n"), uh[0],uh[1],uh[2],uh[3],
		UserHashToString(cID));

	CString str=ipstr(m_dwTraverseIp, ntohs(m_wTraversePort));
	TRACE(_T("\n\n******* buddy dest addr:   %s\n"), (LPCTSTR)str);

	//  header + opcode + kadid&reserve + vc_header + sync + myaddr + myid + connack
	uchar Buf[2+32+1+4+1+6+16+4];
	Buf[0] =OP_EMULEPROT;
	Buf[1] = OP_REASKCALLBACKUDP;

	//  target kad id
	memcpy(Buf+2, pClientSock->client->GetBuddyID(), 16);
	memset(Buf+18,0,16);
	Buf[34] = OP_VC_NAT_HEADER;
	DWORD packsize=17;
	memcpy(Buf+35, &packsize, 4);
	Buf[39] = OP_NAT_SYNC;
	GetMyUserHash((uchar*)Buf+46);
	memcpy(Buf+62, &m_dwConnAskNumber, 4);

	//SendPingPacket();

	return  CNatSocket::SendPacket(m_dwTraverseIp, m_wTraversePort, Buf, 2+32+1+4+1+6+16+4);
}

int CTraverseByBuddy::SendPingPacket()
{
	if(m_dwState & NAT_E_TIMEOUT) return 1;

	m_nPassivePing++;
	m_SendPingTime = time(NULL);

	TRACE("send ping packet for %02X%02X%02X%02X. ConnAck=%08x\n", m_UserHash[0],m_UserHash[1],m_UserHash[2],m_UserHash[3],
		m_dwConnAskNumber);

	if(m_dwClientIp && m_wClientPort)
	{
		uchar pack[20];
		GetMyUserHash(pack);
		memcpy(pack+16, &m_dwConnAskNumber, 4);
		return CNatSocket::SendPacket(m_dwClientIp, m_wClientPort, OP_NAT_PING, pack, 20);
	}

	return -1;
}

bool CTraverseByBuddy::Initialize()
{
	CUpDownClient * client= theApp.clientlist->FindClientByUserHash(m_UserHash);
	if(!client || !client->socket) return false;
	m_Sock = client->socket;

	return true;
}

bool CTraverseByBuddy::ProcessPacket(const uchar * data, int len, DWORD ip, WORD port)
{
	uint8 opcode=data[0];
	//UINT realsize=len-1;// VC-linhai[2007-08-06]:warning C4189: ��realsize�� : �ֲ������ѳ�ʼ����������
	const uchar * realdata=data+1;

	switch(opcode)
	{
	case OP_NAT_FAILED:
		assert(memcmp(realdata+1, m_UserHash, 16)==0);
		if(realdata[0]==1)
		{
			m_dwState |= NAT_E_NOPEER;
			return true;
		}
		break;
	case OP_NAT_SYNC2:
	case OP_NAT_SYNC:
		OnRecvSync(data, len, ip, port);
		return true;
	case OP_NAT_PING:
		OnRecvPing(data, len, ip, port);
		return true;
	}

	return false;
}

void CTraverseByBuddy::OnRecvSync(const uchar * data, int /*len*/, DWORD /*ip*/, WORD /*port*/)
{
	//UINT realsize=len-1;// VC-linhai[2007-08-06]:warning C4189: ��realsize�� : �ֲ������ѳ�ʼ����������
	const uchar * realdata=data+1;

	DWORD cip = *(DWORD*)(realdata);
	WORD cport = *(WORD*)(realdata+4);

	if(cip==0 || cport==0)
	{
		return;
	}
	const uchar * id=realdata+6;

	DWORD ConnAck = 0;
	memcpy(&ConnAck, id+16, 4);

	TRACE("recv sync, ConnAck=%08x\n", ConnAck);
#ifdef _DEBUG
	in_addr addr;
	addr.s_addr = cip;
	TRACE("peer addr of %02X%02X%02X%02X = %s:%d\n", id[0], id[1], id[2],id[3],
		inet_ntoa(addr), ntohs(cport));
#endif
	uchar myid[16];
	GetMyUserHash(myid);
	if(memcmp(id, myid, 16)==0)
	{
		_AddLogLine(false, _T("ERROR: recv myself for traversal"));
		return;
	}

	CAsyncSocketEx * pASock=NULL;
	CUpDownClient * pClient= theApp.clientlist->FindClientByUserHash(id);
	if(pClient)
	{
		pASock = pClient->socket;

		if(! pASock)
		{
			CRuntimeClass * pClassSocket = RUNTIME_CLASS(CClientReqSocket);
			CClientReqSocket * socket = static_cast<CClientReqSocket*>(pClassSocket->CreateObject());
			socket->SetClient(pClient);
			if (!socket->Create())
			{
				socket->Safe_Delete();
				return;
			}

			pClient->socket = socket;
			pASock = socket;
		}
		ASSERT(pASock);
		if(! pASock) return;
	}
	else
	{
		TRACE("\n->%s: Accept a incoming sock for %02X%02X%02X%02X\n", __FUNCTION__,
			id[0],id[1],id[2],id[3]);

		pASock = theApp.natthread->GetTempASock(id);
	}

	m_dwClientIp = cip;
	m_wClientPort = cport;

	m_dwState |=NAT_S_SYNC;
	SendPingPacket();
}

void CTraverseByBuddy::OnRecvPing(const uchar * data, int /*len*/, DWORD ip, WORD port)
{
	//UINT realsize=len-1;// VC-linhai[2007-08-06]:warning C4189: ��realsize�� : �ֲ������ѳ�ʼ����������
	const uchar * realdata=data+1;

	const uchar * hash=realdata;

	DWORD ConnAck=0;
	memcpy(&ConnAck, hash+16, 4);
	TRACE("recv ping 2. %02X%02X%02X%02X, ConnAck=%d\n", hash[0], hash[1], hash[2], hash[3],
		ConnAck);
	CAsyncSocketEx * pASock=NULL;

	if(ConnAck && !m_dwConnAskNumber)
		m_dwConnAskNumber = ConnAck;
	pASock = m_Sock;

	ASSERT(pASock);
	if(!pASock)
	{
		TRACE("no AsyncsocketEx for unconnect sk\n");
		Failed();
		return;
	}

	CNatSocket * nsock = theApp.natthread->FindNatSocket(hash);
	if(nsock)
	{
		Finish();
		return ;
	}

	nsock=new CNatSocket(pASock);
	nsock->m_dwConnAskNumber= m_dwConnAskNumber;
	sUserModeTCPConfig * tc=new sUserModeTCPConfig(ip, port);
	nsock->SetConfig(tc);

//	nsock->m_bTraverseBySvr= true;
//	m_dwTraverseBySvr++;
	nsock->m_TraversalType = CNatSocket::Traversal_bybuddy;

	memcpy(nsock->GetUserHash(), hash, 16);

	theApp.natthread->AddNatSocket(pASock, nsock);
	theApp.natthread->RemoveUnconnSocket(hash);	//ADDED by VC-fengwen on 2007/10/15 : sock��ͨ����unconnectsocket��Ҫ�������

	_AddLogLine(false, _T("NAT traversal connected. %s."), UserHashToString(hash));

	theApp.natthread->RemoveTempASock(hash);

	//pASock->TriggerEvent(FD_CONNECT);
	//pASock->TriggerEvent(FD_WRITE);
	CClientReqSocket * pClientSock = NULL;
	try
	{
		pClientSock = DYNAMIC_DOWNCAST(CClientReqSocket, pASock);
	}
	catch(...)
	{
		TRACE("Exception: %s\n", __FUNCTION__);
		Failed();
		return ;
	}

	if(pClientSock && pClientSock->client)
	{
		pClientSock->client->SetIP(ip);
		pClientSock->client->ResetIP2Country();
	}
	else Failed();

	Finish();
}