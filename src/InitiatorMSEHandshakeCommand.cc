/* <!-- copyright */
/*
 * aria2 - The high speed download utility
 *
 * Copyright (C) 2006 Tatsuhiro Tsujikawa
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 *
 * In addition, as a special exception, the copyright holders give
 * permission to link the code of portions of this program with the
 * OpenSSL library under certain conditions as described in each
 * individual source file, and distribute linked combinations
 * including the two.
 * You must obey the GNU General Public License in all respects
 * for all of the code used other than OpenSSL.  If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so.  If you
 * do not wish to do so, delete this exception statement from your
 * version.  If you delete this exception statement from all source
 * files in the program, then also delete it here.
 */
/* copyright --> */
#include "InitiatorMSEHandshakeCommand.h"
#include "PeerInitiateConnectionCommand.h"
#include "PeerInteractionCommand.h"
#include "DownloadEngine.h"
#include "DlAbortEx.h"
#include "message.h"
#include "prefs.h"
#include "Socket.h"
#include "Logger.h"
#include "Peer.h"
#include "PeerConnection.h"
#include "BtRuntime.h"
#include "PeerStorage.h"
#include "PieceStorage.h"
#include "Option.h"
#include "MSEHandshake.h"
#include "ARC4Encryptor.h"
#include "ARC4Decryptor.h"
#include "RequestGroup.h"
#include "DownloadContext.h"
#include "bittorrent_helper.h"
#include "util.h"

namespace aria2 {

InitiatorMSEHandshakeCommand::InitiatorMSEHandshakeCommand
(cuid_t cuid,
 RequestGroup* requestGroup,
 const SharedHandle<Peer>& p,
 DownloadEngine* e,
 const SharedHandle<BtRuntime>& btRuntime,
 const SharedHandle<SocketCore>& s):

  PeerAbstractCommand(cuid, p, e, s),
  requestGroup_(requestGroup),
  btRuntime_(btRuntime),
  sequence_(INITIATOR_SEND_KEY),
  mseHandshake_(new MSEHandshake(cuid, s, getOption().get()))
{
  disableReadCheckSocket();
  setWriteCheckSocket(getSocket());
  setTimeout(getOption()->getAsInt(PREF_PEER_CONNECTION_TIMEOUT));

  btRuntime_->increaseConnections();
  requestGroup_->increaseNumCommand();
}

InitiatorMSEHandshakeCommand::~InitiatorMSEHandshakeCommand()
{
  requestGroup_->decreaseNumCommand();
  btRuntime_->decreaseConnections();
  
  delete mseHandshake_;
}

bool InitiatorMSEHandshakeCommand::executeInternal() {
  switch(sequence_) {
  case INITIATOR_SEND_KEY: {
    if(!getSocket()->isWritable(0)) {
      break;
    }
    disableWriteCheckSocket();
    setReadCheckSocket(getSocket());
    //socket->setBlockingMode();
    setTimeout(getOption()->getAsInt(PREF_BT_TIMEOUT));
    mseHandshake_->initEncryptionFacility(true);
    if(mseHandshake_->sendPublicKey()) {
      sequence_ = INITIATOR_WAIT_KEY;
    } else {
      setWriteCheckSocket(getSocket());
      sequence_ = INITIATOR_SEND_KEY_PENDING;
    }
    break;
  }
  case INITIATOR_SEND_KEY_PENDING:
    if(mseHandshake_->sendPublicKey()) {
      disableWriteCheckSocket();
      sequence_ = INITIATOR_WAIT_KEY;
    }
    break;
  case INITIATOR_WAIT_KEY: {
    if(mseHandshake_->receivePublicKey()) {
      mseHandshake_->initCipher
        (bittorrent::getInfoHash(requestGroup_->getDownloadContext()));;
      if(mseHandshake_->sendInitiatorStep2()) {
        sequence_ = INITIATOR_FIND_VC_MARKER;
      } else {
        setWriteCheckSocket(getSocket());
        sequence_ = INITIATOR_SEND_STEP2_PENDING;
      }
    }
    break;
  }
  case INITIATOR_SEND_STEP2_PENDING:
    if(mseHandshake_->sendInitiatorStep2()) {
      disableWriteCheckSocket();
      sequence_ = INITIATOR_FIND_VC_MARKER;
    }
    break;
  case INITIATOR_FIND_VC_MARKER: {
    if(mseHandshake_->findInitiatorVCMarker()) {
      sequence_ = INITIATOR_RECEIVE_PAD_D_LENGTH;
    }
    break;
  }
  case INITIATOR_RECEIVE_PAD_D_LENGTH: {
    if(mseHandshake_->receiveInitiatorCryptoSelectAndPadDLength()) {
      sequence_ = INITIATOR_RECEIVE_PAD_D;
    }
    break;
  }
  case INITIATOR_RECEIVE_PAD_D: {
    if(mseHandshake_->receivePad()) {
      SharedHandle<PeerConnection> peerConnection
        (new PeerConnection(getCuid(), getPeer(), getSocket()));
      if(mseHandshake_->getNegotiatedCryptoType() == MSEHandshake::CRYPTO_ARC4){
        peerConnection->enableEncryption(mseHandshake_->getEncryptor(),
                                         mseHandshake_->getDecryptor());
      }
      PeerInteractionCommand* c =
        new PeerInteractionCommand
        (getCuid(), requestGroup_, getPeer(), getDownloadEngine(), btRuntime_,
         pieceStorage_,
         peerStorage_,
         getSocket(),
         PeerInteractionCommand::INITIATOR_SEND_HANDSHAKE,
         peerConnection);
      getDownloadEngine()->addCommand(c);
      return true;
    }
    break;
  }
  }
  getDownloadEngine()->addCommand(this);
  return false;
}

bool InitiatorMSEHandshakeCommand::prepareForNextPeer(time_t wait)
{
  if(getOption()->getAsBool(PREF_BT_REQUIRE_CRYPTO)) {
    if(getLogger()->info()) {
      getLogger()->info
        ("CUID#%s - Establishing connection using legacy BitTorrent"
         " handshake is disabled by preference.",
         util::itos(getCuid()).c_str());
    }
    if(peerStorage_->isPeerAvailable() && btRuntime_->lessThanEqMinPeers()) {
      SharedHandle<Peer> peer = peerStorage_->getUnusedPeer();
      peer->usedBy(getDownloadEngine()->newCUID());
      PeerInitiateConnectionCommand* command =
        new PeerInitiateConnectionCommand(peer->usedBy(), requestGroup_, peer,
                                          getDownloadEngine(), btRuntime_);
      command->setPeerStorage(peerStorage_);
      command->setPieceStorage(pieceStorage_);
      getDownloadEngine()->addCommand(command);
    }
    return true;
  } else {
    // try legacy BitTorrent handshake
    if(getLogger()->info()) {
      getLogger()->info("CUID#%s - Retry using legacy BitTorrent handshake.",
                        util::itos(getCuid()).c_str());
    }
    PeerInitiateConnectionCommand* command =
      new PeerInitiateConnectionCommand(getCuid(), requestGroup_, getPeer(),
                                        getDownloadEngine(), btRuntime_, false);
    command->setPeerStorage(peerStorage_);
    command->setPieceStorage(pieceStorage_);
    getDownloadEngine()->addCommand(command);
    return true;
  }
}

void InitiatorMSEHandshakeCommand::onAbort()
{
  if(getOption()->getAsBool(PREF_BT_REQUIRE_CRYPTO)) {
    peerStorage_->returnPeer(getPeer());
  }
}

bool InitiatorMSEHandshakeCommand::exitBeforeExecute()
{
  return btRuntime_->isHalt();
}

void InitiatorMSEHandshakeCommand::setPeerStorage
(const SharedHandle<PeerStorage>& peerStorage)
{
  peerStorage_ = peerStorage;
}

void InitiatorMSEHandshakeCommand::setPieceStorage
(const SharedHandle<PieceStorage>& pieceStorage)
{
  pieceStorage_ = pieceStorage;
}

const SharedHandle<Option>& InitiatorMSEHandshakeCommand::getOption() const
{
  return requestGroup_->getOption();
}

} // namespace aria2
