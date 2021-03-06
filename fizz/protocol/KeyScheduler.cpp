/*
 *  Copyright (c) 2018-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree.
 */

#include <fizz/protocol/KeyScheduler.h>

using folly::StringPiece;

static constexpr StringPiece kTrafficKey{"key"};
static constexpr StringPiece kTrafficIv{"iv"};

static constexpr StringPiece kExternalPskBinder{"ext binder"};
static constexpr StringPiece kResumptionPskBinder{"res binder"};
static constexpr StringPiece kClientEarlyTraffic{"c e traffic"};
static constexpr StringPiece kEarlyExporter{"e exp master"};
static constexpr StringPiece kClientHandshakeTraffic{"c hs traffic"};
static constexpr StringPiece kServerHandshakeTraffic{"s hs traffic"};
static constexpr StringPiece kClientAppTraffic{"c ap traffic"};
static constexpr StringPiece kServerAppTraffic{"s ap traffic"};
static constexpr StringPiece kExporterMaster{"exp master"};
static constexpr StringPiece kResumptionMaster{"res master"};
static constexpr StringPiece kDerivedSecret{"derived"};
static constexpr StringPiece kTrafficKeyUpdate{"traffic upd"};
static constexpr StringPiece kResumption{"resumption"};

namespace fizz {

void KeyScheduler::deriveEarlySecret(folly::ByteRange psk) {
  if (secret_) {
    throw std::runtime_error("secret already set");
  }

  auto zeros = std::vector<uint8_t>(deriver_->hashLength(), 0);
  secret_ = EarlySecret{deriver_->hkdfExtract(folly::range(zeros), psk)};
}

void KeyScheduler::deriveHandshakeSecret() {
  auto& earlySecret = boost::get<EarlySecret>(*secret_);
  auto zeros = std::vector<uint8_t>(deriver_->hashLength(), 0);
  auto preSecret = deriver_->deriveSecret(
      folly::range(earlySecret.secret), kDerivedSecret, deriver_->blankHash());
  secret_ = HandshakeSecret{
      deriver_->hkdfExtract(folly::range(preSecret), folly::range(zeros))};
}

void KeyScheduler::deriveHandshakeSecret(folly::ByteRange ecdhe) {
  if (!secret_) {
    auto zeros = std::vector<uint8_t>(deriver_->hashLength(), 0);
    secret_ = EarlySecret{
        deriver_->hkdfExtract(folly::range(zeros), folly::range(zeros))};
  }

  auto& earlySecret = boost::get<EarlySecret>(*secret_);
  auto preSecret = deriver_->deriveSecret(
      folly::range(earlySecret.secret), kDerivedSecret, deriver_->blankHash());
  secret_ =
      HandshakeSecret{deriver_->hkdfExtract(folly::range(preSecret), ecdhe)};
}

void KeyScheduler::deriveMasterSecret() {
  auto zeros = std::vector<uint8_t>(deriver_->hashLength(), 0);
  auto& handshakeSecret = boost::get<HandshakeSecret>(*secret_);
  auto preSecret = deriver_->deriveSecret(
      folly::range(handshakeSecret.secret),
      kDerivedSecret,
      deriver_->blankHash());
  secret_ = MasterSecret{
      deriver_->hkdfExtract(folly::range(preSecret), folly::range(zeros))};
}

void KeyScheduler::deriveAppTrafficSecrets(folly::ByteRange transcript) {
  auto& masterSecret = boost::get<MasterSecret>(*secret_);
  AppTrafficSecret trafficSecret;
  trafficSecret.client = deriver_->deriveSecret(
      folly::range(masterSecret.secret), kClientAppTraffic, transcript);
  trafficSecret.server = deriver_->deriveSecret(
      folly::range(masterSecret.secret), kServerAppTraffic, transcript);
  appTrafficSecret_ = std::move(trafficSecret);
}

void KeyScheduler::clearMasterSecret() {
  boost::get<MasterSecret>(*secret_);
  secret_ = folly::none;
}

uint32_t KeyScheduler::clientKeyUpdate() {
  auto& appTrafficSecret = *appTrafficSecret_;
  auto buf = deriver_->expandLabel(
      folly::range(appTrafficSecret.client),
      kTrafficKeyUpdate,
      folly::IOBuf::create(0),
      deriver_->hashLength());
  buf->coalesce();
  appTrafficSecret.client = std::vector<uint8_t>(buf->data(), buf->tail());
  return ++appTrafficSecret.clientGeneration;
}

uint32_t KeyScheduler::serverKeyUpdate() {
  auto& appTrafficSecret = *appTrafficSecret_;
  auto buf = deriver_->expandLabel(
      folly::range(appTrafficSecret.server),
      kTrafficKeyUpdate,
      folly::IOBuf::create(0),
      deriver_->hashLength());
  buf->coalesce();
  appTrafficSecret.server = std::vector<uint8_t>(buf->data(), buf->tail());
  return ++appTrafficSecret.serverGeneration;
}

std::vector<uint8_t> KeyScheduler::getSecret(
    EarlySecrets s,
    folly::ByteRange transcript) const {
  StringPiece label;
  switch (s) {
    case EarlySecrets::ExternalPskBinder:
      label = kExternalPskBinder;
      break;
    case EarlySecrets::ResumptionPskBinder:
      label = kResumptionPskBinder;
      break;
    case EarlySecrets::ClientEarlyTraffic:
      label = kClientEarlyTraffic;
      break;
    case EarlySecrets::EarlyExporter:
      label = kEarlyExporter;
      break;
    default:
      LOG(FATAL) << "unknown secret";
  }

  auto& earlySecret = boost::get<EarlySecret>(*secret_);
  return deriver_->deriveSecret(
      folly::range(earlySecret.secret), label, transcript);
}

std::vector<uint8_t> KeyScheduler::getSecret(
    HandshakeSecrets s,
    folly::ByteRange transcript) const {
  StringPiece label;
  switch (s) {
    case HandshakeSecrets::ClientHandshakeTraffic:
      label = kClientHandshakeTraffic;
      break;
    case HandshakeSecrets::ServerHandshakeTraffic:
      label = kServerHandshakeTraffic;
      break;
    default:
      LOG(FATAL) << "unknown secret";
  }

  auto& handshakeSecret = boost::get<HandshakeSecret>(*secret_);
  return deriver_->deriveSecret(
      folly::range(handshakeSecret.secret), label, transcript);
}

std::vector<uint8_t> KeyScheduler::getSecret(
    MasterSecrets s,
    folly::ByteRange transcript) const {
  StringPiece label;
  switch (s) {
    case MasterSecrets::ExporterMaster:
      label = kExporterMaster;
      break;
    case MasterSecrets::ResumptionMaster:
      label = kResumptionMaster;
      break;
    default:
      LOG(FATAL) << "unknown secret";
  }

  auto& masterSecret = boost::get<MasterSecret>(*secret_);
  return deriver_->deriveSecret(
      folly::range(masterSecret.secret), label, transcript);
}

std::vector<uint8_t> KeyScheduler::getSecret(AppTrafficSecrets s) const {
  auto& appTrafficSecret = *appTrafficSecret_;
  switch (s) {
    case AppTrafficSecrets::ClientAppTraffic:
      return appTrafficSecret.client;
    case AppTrafficSecrets::ServerAppTraffic:
      return appTrafficSecret.server;
    default:
      LOG(FATAL) << "unknown secret";
  }
}

TrafficKey KeyScheduler::getTrafficKey(
    folly::ByteRange trafficSecret,
    size_t keyLength,
    size_t ivLength) const {
  TrafficKey trafficKey;
  trafficKey.key = deriver_->expandLabel(
      trafficSecret, kTrafficKey, folly::IOBuf::create(0), keyLength);
  trafficKey.iv = deriver_->expandLabel(
      trafficSecret, kTrafficIv, folly::IOBuf::create(0), ivLength);
  return trafficKey;
}

Buf KeyScheduler::getResumptionSecret(
    folly::ByteRange resumptionMasterSecret,
    folly::ByteRange ticketNonce) const {
  return deriver_->expandLabel(
      resumptionMasterSecret,
      kResumption,
      folly::IOBuf::wrapBuffer(ticketNonce),
      deriver_->hashLength());
}
} // namespace fizz
