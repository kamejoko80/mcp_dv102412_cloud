/*****************************************************************************
*
*  exosite_hal.c - Exosite hardware & environmenat adapation layer.
*  Copyright (C) 2012 Exosite LLC
*
*  Redistribution and use in source and binary forms, with or without
*  modification, are permitted provided that the following conditions
*  are met:
*
*    Redistributions of source code must retain the above copyright
*    notice, this list of conditions and the following disclaimer.
*
*    Redistributions in binary form must reproduce the above copyright
*    notice, this list of conditions and the following disclaimer in the
*    documentation and/or other materials provided with the   
*    distribution.
*
*    Neither the name of Texas Instruments Incorporated nor the names of
*    its contributors may be used to endorse or promote products derived
*    from this software without specific prior written permission.
*
*  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS 
*  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT 
*  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
*  A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT 
*  OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, 
*  SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT 
*  LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
*  DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
*  THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT 
*  (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE 
*  OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*
*****************************************************************************/
#include "exosite.h"
#include "exosite_hal.h"
#include "exosite_meta.h"
#include "TCPIPConfig.h"
#include "TCPIP Stack/TCPIP.h"
#include "stdlib.h"

// local variables

char exometa[META_SIZE];
const exosite_meta meta_struct;

// local functions

// externs
//extern char *itoa(int n, char *s, int b);
extern APP_CONFIG AppConfig;

// global variables
#define EXOMETA_ADDR 0xbd050000 //0x9d037000
TCP_SOCKET exSocket = INVALID_SOCKET;
int wait_count = 0;
int wait_response = 0;
int recv_done = 0;
int send_end = 0;
DWORD Timer;
static enum _GenericTCPState
{
  EX_HOME = 0,
  EX_SOCKET_OBTAINED,
  EX_PACKAGE_SEND,
  EX_PROCESS_RESPONSE,
  EX_DISCONNECT,
  EX_DONE
} GenericTCPState = EX_DONE;


/*****************************************************************************
*
*  exoHAL_ReadHWMAC
*
*  \param  Interface Number (1 - WiFi), buffer to return hexadecimal MAC
*
*  \return 0 if failure; len of UUID if success;
*
*  \brief  Reads the MAC address from the hardware
*
*****************************************************************************/
int
exoHAL_ReadUUID(unsigned char if_nbr, unsigned char * UUID_buf)
{
  int retval = 0;
  MAC_ADDR macbuf;
  unsigned char macstr[20];

  switch (if_nbr) {
    case IF_GPRS:
      break;
    case IF_ENET:
      break;
    case IF_WIFI:
      WF_GetMacAddress(macbuf.v);
      sprintf((char *)macstr,"%02X%02X%02X%02X%02X%02X", macbuf.v[0], macbuf.v[1], macbuf.v[2], macbuf.v[3], macbuf.v[4], macbuf.v[5]);
      retval = strlen((char *)macstr);
      memcpy(UUID_buf, macstr, retval);
      UUID_buf[retval] = 0;
      break;
    default:
      break;
  }

  return retval;
}


/*****************************************************************************
*
* exoHAL_EnableNVMeta
*
*  \param  None
*
*  \return None
*
*  \brief  Enables meta non-volatile memory, if any
*
*****************************************************************************/
void
exoHAL_EnableMeta(void)
{
  return;
}


/*****************************************************************************
*
*  exoHAL_EraseNVMeta
*
*  \param  None
*
*  \return None
*
*  \brief  Wipes out meta information - replaces with 0's
*
*****************************************************************************/
void
exoHAL_EraseMeta(void)
{
  NVMErasePage((void *)EXOMETA_ADDR);

  return;
}


/*****************************************************************************
*
*  exoHAL_WriteMetaItem
*
*  \param  buffer - string buffer containing info to write to meta; len -
*          size of string in bytes; offset - offset from base of meta
*          location to store the item
*
*  \return None
*
*  \brief  Stores information to the NV meta structure
*
*****************************************************************************/
void
exoHAL_WriteMetaItem(unsigned char * buffer, unsigned char len, int offset)
{
  int i;
  DWORD address = EXOMETA_ADDR + offset;
  DWORD input_tmp;

  if (len % 2 > 0)
    len += 1;
  for(i = 0; i < len;)
  {
    input_tmp = (buffer[i + 3] << 24 & 0xff000000) | 
                (buffer[i + 2] << 16 & 0xff0000) |
                (buffer[i + 1] << 8 & 0xff00) |
                (buffer[i] & 0xff);
    i += 4;
    NVMWriteWord((void *)(address), input_tmp);
    address += 4;
  }

  return;
}


/*****************************************************************************
*
*  exoHAL_ReadMetaItem
*
*  \param  buffer - buffer we can read meta info into; len - size of the
*          buffer (max 256 bytes); offset - offset from base of meta to begin
*          reading from;
*
*  \return None
*
*  \brief  Reads information from the NV meta structure
*
*****************************************************************************/
void
exoHAL_ReadMetaItem(unsigned char * buffer, unsigned char len, int offset)
{
  DWORD address = EXOMETA_ADDR + offset;
  memcpy(buffer, (int *)(address), len);

  return;
}


/*****************************************************************************
*
*  exoHAL_SocketClose
*
*  \param  socket - socket handle
*
*  \return None
*
*  \brief  Closes a socket
*
*****************************************************************************/
void
exoHAL_SocketClose(long socket)
{
  // Send everything immediately
  if (GenericTCPState == EX_DISCONNECT)
  {
    if (TCPIsConnected((TCP_SOCKET)socket) && recv_done == 1)
    {
      TCPDisconnect((TCP_SOCKET)socket);
      recv_done = 0;
      exSocket = INVALID_SOCKET;
    }
    wait_response = 0;
    send_end = 0;
    GenericTCPState = EX_DONE;
  }
  return;
}


/*****************************************************************************
*
*  exoHAL_SocketOpenTCP
*
*  \param  None
*
*  \return -1: failure; Other: socket handle
*
*  \brief  Opens a TCP socket
*
*****************************************************************************/
long
exoHAL_SocketOpenTCP(unsigned char *server)
{
  DWORD HexIP = 0;
  int HexPort = server[5] & 0xff;

  HexIP = (server[3] << 24 & 0xff000000) | (server[2] << 16 & 0xff0000)
          | (server[1] << 8 & 0xff00) | (server[0] & 0xff);

  if (GenericTCPState == EX_DONE)
    GenericTCPState = EX_HOME;

  // Start the TCP server, listening on RX_PERFORMANCE_PORT
  if (GenericTCPState == EX_HOME)
  {
    if (exSocket == INVALID_SOCKET)
    {
      exSocket = TCPOpen(HexIP, TCP_OPEN_IP_ADDRESS, HexPort, TCP_PURPOSE_GENERIC_TCP_CLIENT);

      if (exSocket == INVALID_SOCKET)
      {
        return -1;
      }
    }
    GenericTCPState = EX_SOCKET_OBTAINED;
  }
  return (long)exSocket;
}


/*****************************************************************************
*
*  exoHAL_ServerConnect
*
*  \param  None
*
*  \return socket - socket handle
*
*  \brief  The function opens a TCP socket
*
*****************************************************************************/
long
exoHAL_ServerConnect(long sock)
{
  if (GenericTCPState == EX_SOCKET_OBTAINED)
  {
    GenericTCPState++;
  }

  return (long)sock;
}

/*****************************************************************************
*
*  exoHAL_SocketSend
*
*  \param  socket - socket handle; buffer - string buffer containing info to
*          send; len - size of string in bytes;
*
*  \return Number of bytes sent
*
*  \brief  Sends data out to the internet
*
*****************************************************************************/
unsigned char
exoHAL_SocketSend(long socket, char * buffer, unsigned char len)
{
  int send_len = 0;

  if (GenericTCPState == EX_PACKAGE_SEND)
  {
    if (TCPIsConnected((TCP_SOCKET)socket))
    {
      char tempbuf[150];
      memcpy(tempbuf, buffer, len);
      send_len = TCPPutArray((TCP_SOCKET)socket, (BYTE *)buffer, len);
      send_end ++;
      wait_count = 0;
    }
    else
    {
      wait_count++;
      if (wait_count > 10)
      {
        recv_done = 1;
        GenericTCPState = EX_DISCONNECT;
        wait_count = 0;
      }
    }
  }

  return len;
}


/*****************************************************************************
*
*  exoHAL_SocketRecv
*
*  \param  socket - socket handle; buffer - string buffer to put info we
*          receive; len - size of buffer in bytes;
*
*  \return Number of bytes received
*
*  \brief  Receives data from the internet
*
*****************************************************************************/
unsigned char
exoHAL_SocketRecv(long socket, char * buffer, unsigned char len)
{
  WORD w, wGetLen;

  if (!TCPIsConnected((TCP_SOCKET)socket))
  {
    wait_count++;
    if (wait_count > 10)
    {
      recv_done = 1;
      GenericTCPState = EX_DISCONNECT;
      wait_count = 0;
    }

    return 0;
  }
  else wait_count = 0;

  if (GenericTCPState == EX_PACKAGE_SEND && send_end >= 4)
  {
    GenericTCPState++;
  }

  if (GenericTCPState == EX_PROCESS_RESPONSE)
  {
    wait_response++;
    if (!TCPIsGetReady(socket))
    {
      return 0;
    }
    w = TCPIsGetReady((TCP_SOCKET)socket);
    buffer[0] = 0;
    if ( w )
    {
      wGetLen = w;
      TCPGetArray((TCP_SOCKET)socket, (BYTE *)buffer, len);
      if (send_end >= 4 && wait_response > 0 && w < len)
      {
        GenericTCPState = EX_DISCONNECT;
      }

      return len;
    }
  }

  return 0;
}


/*****************************************************************************
*
*  exoHAL_MSDelay
*
*  \param  delay - milliseconds to delay
*
*  \return None
*
*  \brief  Delays for specified milliseconds
*
*****************************************************************************/
void
exoHAL_MSDelay(unsigned short delay)
{

  return;
}
