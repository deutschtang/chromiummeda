// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/mojo/services/mojo_cdm.h"

#include "base/bind.h"
#include "base/bind_helpers.h"
#include "media/base/cdm_key_information.h"
#include "media/base/cdm_promise.h"
#include "media/mojo/services/media_type_converters.h"
#include "third_party/mojo/src/mojo/public/cpp/application/connect.h"
#include "third_party/mojo/src/mojo/public/cpp/bindings/interface_impl.h"
#include "third_party/mojo/src/mojo/public/interfaces/application/service_provider.mojom.h"
#include "url/gurl.h"

namespace media {

template <typename PromiseType>
static void RejectPromise(scoped_ptr<PromiseType> promise,
                          mojo::CdmPromiseResultPtr result) {
  promise->reject(static_cast<MediaKeys::Exception>(result->exception),
                  result->system_code, result->error_message);
}

MojoCdm::MojoCdm(mojo::ContentDecryptionModulePtr remote_cdm,
                 const SessionMessageCB& session_message_cb,
                 const SessionClosedCB& session_closed_cb,
                 const LegacySessionErrorCB& legacy_session_error_cb,
                 const SessionKeysChangeCB& session_keys_change_cb,
                 const SessionExpirationUpdateCB& session_expiration_update_cb)
    : remote_cdm_(remote_cdm.Pass()),
      binding_(this),
      session_message_cb_(session_message_cb),
      session_closed_cb_(session_closed_cb),
      legacy_session_error_cb_(legacy_session_error_cb),
      session_keys_change_cb_(session_keys_change_cb),
      session_expiration_update_cb_(session_expiration_update_cb),
      weak_factory_(this) {
  DVLOG(1) << __FUNCTION__;
  DCHECK(!session_message_cb_.is_null());
  DCHECK(!session_closed_cb_.is_null());
  DCHECK(!legacy_session_error_cb_.is_null());
  DCHECK(!session_keys_change_cb_.is_null());
  DCHECK(!session_expiration_update_cb_.is_null());

  mojo::ContentDecryptionModuleClientPtr client_ptr;
  binding_.Bind(GetProxy(&client_ptr));
  remote_cdm_->SetClient(client_ptr.Pass());
}

MojoCdm::~MojoCdm() {
  DVLOG(1) << __FUNCTION__;
}

void MojoCdm::SetServerCertificate(const std::vector<uint8_t>& certificate,
                                   scoped_ptr<SimpleCdmPromise> promise) {
  remote_cdm_->SetServerCertificate(
      mojo::Array<uint8_t>::From(certificate),
      base::Bind(&MojoCdm::OnPromiseResult<>, weak_factory_.GetWeakPtr(),
                 base::Passed(&promise)));
}

void MojoCdm::CreateSessionAndGenerateRequest(
    SessionType session_type,
    EmeInitDataType init_data_type,
    const std::vector<uint8_t>& init_data,
    scoped_ptr<NewSessionCdmPromise> promise) {
  remote_cdm_->CreateSessionAndGenerateRequest(
      static_cast<mojo::ContentDecryptionModule::SessionType>(session_type),
      static_cast<mojo::ContentDecryptionModule::InitDataType>(init_data_type),
      mojo::Array<uint8_t>::From(init_data),
      base::Bind(&MojoCdm::OnPromiseResult<std::string>,
                 weak_factory_.GetWeakPtr(), base::Passed(&promise)));
}

void MojoCdm::LoadSession(SessionType session_type,
                          const std::string& session_id,
                          scoped_ptr<NewSessionCdmPromise> promise) {
  remote_cdm_->LoadSession(
      static_cast<mojo::ContentDecryptionModule::SessionType>(session_type),
      session_id,
      base::Bind(&MojoCdm::OnPromiseResult<std::string>,
                 weak_factory_.GetWeakPtr(), base::Passed(&promise)));
}

void MojoCdm::UpdateSession(const std::string& session_id,
                            const std::vector<uint8_t>& response,
                            scoped_ptr<SimpleCdmPromise> promise) {
  remote_cdm_->UpdateSession(
      session_id, mojo::Array<uint8_t>::From(response),
      base::Bind(&MojoCdm::OnPromiseResult<>, weak_factory_.GetWeakPtr(),
                 base::Passed(&promise)));
}

void MojoCdm::CloseSession(const std::string& session_id,
                           scoped_ptr<SimpleCdmPromise> promise) {
  remote_cdm_->CloseSession(session_id, base::Bind(&MojoCdm::OnPromiseResult<>,
                                                   weak_factory_.GetWeakPtr(),
                                                   base::Passed(&promise)));
}

void MojoCdm::RemoveSession(const std::string& session_id,
                            scoped_ptr<SimpleCdmPromise> promise) {
  remote_cdm_->RemoveSession(session_id, base::Bind(&MojoCdm::OnPromiseResult<>,
                                                    weak_factory_.GetWeakPtr(),
                                                    base::Passed(&promise)));
}

CdmContext* MojoCdm::GetCdmContext() {
  NOTIMPLEMENTED();
  return nullptr;
}

void MojoCdm::OnSessionMessage(const mojo::String& session_id,
                               mojo::CdmMessageType message_type,
                               mojo::Array<uint8_t> message,
                               const mojo::String& legacy_destination_url) {
  GURL verified_gurl = GURL(legacy_destination_url);
  if (!verified_gurl.is_valid() && !verified_gurl.is_empty()) {
    DLOG(WARNING) << "SessionMessage destination_url is invalid : "
                  << verified_gurl.possibly_invalid_spec();
    verified_gurl = GURL::EmptyGURL();  // Replace invalid destination_url.
  }

  session_message_cb_.Run(session_id,
                          static_cast<MediaKeys::MessageType>(message_type),
                          message.storage(), verified_gurl);
}

void MojoCdm::OnSessionClosed(const mojo::String& session_id) {
  session_closed_cb_.Run(session_id);
}

void MojoCdm::OnLegacySessionError(const mojo::String& session_id,
                                   mojo::CdmException exception,
                                   uint32_t system_code,
                                   const mojo::String& error_message) {
  legacy_session_error_cb_.Run(session_id,
                               static_cast<MediaKeys::Exception>(exception),
                               system_code, error_message);
}

void MojoCdm::OnSessionKeysChange(
    const mojo::String& session_id,
    bool has_additional_usable_key,
    mojo::Array<mojo::CdmKeyInformationPtr> keys_info) {
  media::CdmKeysInfo key_data;
  key_data.reserve(keys_info.size());
  for (size_t i = 0; i < keys_info.size(); ++i) {
    key_data.push_back(
        keys_info[i].To<scoped_ptr<media::CdmKeyInformation>>().release());
  }
  session_keys_change_cb_.Run(session_id, has_additional_usable_key,
                              key_data.Pass());
}

void MojoCdm::OnSessionExpirationUpdate(const mojo::String& session_id,
                                        double new_expiry_time_sec) {
  session_expiration_update_cb_.Run(
      session_id, base::Time::FromDoubleT(new_expiry_time_sec));
}

}  // namespace media
