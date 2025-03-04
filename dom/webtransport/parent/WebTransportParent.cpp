/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "WebTransportParent.h"
#include "Http3WebTransportSession.h"
#include "mozilla/StaticPrefs_network.h"
#include "mozilla/TimeStamp.h"
#include "mozilla/Unused.h"
#include "mozilla/dom/ClientInfo.h"
#include "mozilla/dom/WebTransportBinding.h"
#include "mozilla/dom/WebTransportLog.h"
#include "mozilla/ipc/BackgroundParent.h"
#include "nsIEventTarget.h"
#include "nsIOService.h"
#include "nsIPrincipal.h"
#include "nsIWebTransport.h"
#include "nsStreamUtils.h"
#include "nsIWebTransportStream.h"

using IPCResult = mozilla::ipc::IPCResult;

namespace mozilla::dom {

NS_IMPL_ISUPPORTS(WebTransportParent, WebTransportSessionEventListener);

using CreateWebTransportPromise =
    MozPromise<WebTransportReliabilityMode, nsresult, true>;
WebTransportParent::~WebTransportParent() {
  LOG(("Destroying WebTransportParent %p", this));
}

void WebTransportParent::Create(
    const nsAString& aURL, nsIPrincipal* aPrincipal,
    const mozilla::Maybe<IPCClientInfo>& aClientInfo, const bool& aDedicated,
    const bool& aRequireUnreliable, const uint32_t& aCongestionControl,
    // Sequence<WebTransportHash>* aServerCertHashes,
    Endpoint<PWebTransportParent>&& aParentEndpoint,
    std::function<void(std::tuple<const nsresult&, const uint8_t&>)>&&
        aResolver) {
  LOG(("Created WebTransportParent %p %s %s %s congestion=%s", this,
       NS_ConvertUTF16toUTF8(aURL).get(),
       aDedicated ? "Dedicated" : "AllowPooling",
       aRequireUnreliable ? "RequireUnreliable" : "",
       aCongestionControl ==
               (uint32_t)dom::WebTransportCongestionControl::Throughput
           ? "ThroughPut"
           : (aCongestionControl ==
                      (uint32_t)dom::WebTransportCongestionControl::Low_latency
                  ? "Low-Latency"
                  : "Default")));

  if (!StaticPrefs::network_webtransport_enabled()) {
    aResolver(ResolveType(
        NS_ERROR_DOM_NOT_ALLOWED_ERR,
        static_cast<uint8_t>(WebTransportReliabilityMode::Pending)));
    return;
  }

  if (!aParentEndpoint.IsValid()) {
    aResolver(ResolveType(
        NS_ERROR_INVALID_ARG,
        static_cast<uint8_t>(WebTransportReliabilityMode::Pending)));
    return;
  }

  MOZ_DIAGNOSTIC_ASSERT(mozilla::net::gIOService);
  nsresult rv =
      mozilla::net::gIOService->NewWebTransport(getter_AddRefs(mWebTransport));
  if (NS_FAILED(rv)) {
    aResolver(ResolveType(
        rv, static_cast<uint8_t>(WebTransportReliabilityMode::Pending)));
    return;
  }

  mOwningEventTarget = GetCurrentSerialEventTarget();
  MOZ_ASSERT(aPrincipal);
  nsCOMPtr<nsIURI> uri;
  rv = NS_NewURI(getter_AddRefs(uri), aURL);
  if (NS_FAILED(rv)) {
    aResolver(ResolveType(
        NS_ERROR_INVALID_ARG,
        static_cast<uint8_t>(WebTransportReliabilityMode::Pending)));
    return;
  }

  nsCOMPtr<nsIRunnable> r = NS_NewRunnableFunction(
      "WebTransport AsyncConnect",
      [self = RefPtr{this}, uri = std::move(uri),
       principal = RefPtr{aPrincipal},
       flags = nsILoadInfo::SEC_ALLOW_CROSS_ORIGIN_SEC_CONTEXT_IS_NULL,
       clientInfo = aClientInfo] {
        LOG(("WebTransport %p AsyncConnect", self.get()));
        if (NS_FAILED(self->mWebTransport->AsyncConnectWithClient(
                uri, principal, flags, self, clientInfo))) {
          LOG(("AsyncConnect failure; we should get OnSessionClosed"));
        }
      });

  // Bind to SocketThread for IPC - connection creation/destruction must
  // hit MainThread, but keep all other traffic on SocketThread.  Note that
  // we must call aResolver() on this (PBackground) thread.
  mSocketThread = do_GetService(NS_SOCKETTRANSPORTSERVICE_CONTRACTID, &rv);
  MOZ_ASSERT(NS_SUCCEEDED(rv));

  InvokeAsync(mSocketThread, __func__,
              [parentEndpoint = std::move(aParentEndpoint), runnable = r,
               resolver = std::move(aResolver), p = RefPtr{this}]() mutable {
                {
                  MutexAutoLock lock(p->mMutex);
                  p->mResolver = resolver;
                }

                LOG(("Binding parent endpoint"));
                if (!parentEndpoint.Bind(p)) {
                  return CreateWebTransportPromise::CreateAndReject(
                      NS_ERROR_FAILURE, __func__);
                }
                // IPC now holds a ref to parent
                // Send connection to the server via MainThread
                NS_DispatchToMainThread(runnable, NS_DISPATCH_NORMAL);

                return CreateWebTransportPromise::CreateAndResolve(
                    WebTransportReliabilityMode::Supports_unreliable, __func__);
              })
      ->Then(
          GetCurrentSerialEventTarget(), __func__,
          [p = RefPtr{this}](
              const CreateWebTransportPromise::ResolveOrRejectValue& aValue) {
            if (aValue.IsReject()) {
              std::function<void(ResolveType)> resolver;
              {
                MutexAutoLock lock(p->mMutex);
                resolver = std::move(p->mResolver);
              }
              if (resolver) {
                resolver(
                    ResolveType(aValue.RejectValue(),
                                static_cast<uint8_t>(
                                    WebTransportReliabilityMode::Pending)));
              }
            }
          });
}

void WebTransportParent::ActorDestroy(ActorDestroyReason aWhy) {
  LOG(("ActorDestroy WebTransportParent %d", aWhy));
}

// We may not receive this response if the child side is destroyed without
// `Close` or `Shutdown` being explicitly called.
IPCResult WebTransportParent::RecvClose(const uint32_t& aCode,
                                        const nsACString& aReason) {
  LOG(("Close for %p received, code = %u, reason = %s", this, aCode,
       PromiseFlatCString(aReason).get()));
  {
    MutexAutoLock lock(mMutex);
    MOZ_ASSERT(!mClosed);
    mClosed.Flip();
  }
  mWebTransport->CloseSession(aCode, aReason);
  Close();
  return IPC_OK();
}

class ReceiveStream final : public nsIWebTransportStreamCallback {
 public:
  NS_DECL_THREADSAFE_ISUPPORTS
  NS_DECL_NSIWEBTRANSPORTSTREAMCALLBACK

  ReceiveStream(
      WebTransportParent::CreateUnidirectionalStreamResolver&& aResolver,
      nsCOMPtr<nsISerialEventTarget>& aSocketThread)
      : mUniResolver(aResolver), mSocketThread(aSocketThread) {}
  ReceiveStream(
      WebTransportParent::CreateBidirectionalStreamResolver&& aResolver,
      nsCOMPtr<nsISerialEventTarget>& aSocketThread)
      : mBiResolver(aResolver), mSocketThread(aSocketThread) {}

 private:
  ~ReceiveStream() = default;
  std::function<void(::mozilla::ipc::DataPipeSender*)> mUniResolver;
  WebTransportParent::CreateBidirectionalStreamResolver mBiResolver;
  nsCOMPtr<nsISerialEventTarget> mSocketThread;
};

NS_IMPL_ISUPPORTS(ReceiveStream, nsIWebTransportStreamCallback)

// nsIWebTransportStreamCallback:
NS_IMETHODIMP ReceiveStream::OnBidirectionalStreamReady(
    nsIWebTransportBidirectionalStream* aStream) {
  LOG(("Bidirectional stream ready!"));
  MOZ_ASSERT(mSocketThread->IsOnCurrentThread());

  RefPtr<mozilla::ipc::DataPipeSender> inputsender;
  RefPtr<mozilla::ipc::DataPipeReceiver> inputreceiver;
  nsresult rv =
      NewDataPipe(mozilla::ipc::kDefaultDataPipeCapacity,
                  getter_AddRefs(inputsender), getter_AddRefs(inputreceiver));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    mBiResolver(rv);
    return rv;
  }

  nsCOMPtr<nsIAsyncInputStream> inputStream;
  aStream->GetInputStream(getter_AddRefs(inputStream));
  MOZ_ASSERT(inputStream);
  rv = NS_AsyncCopy(inputStream, inputsender, mSocketThread,
                    NS_ASYNCCOPY_VIA_WRITESEGMENTS,  // can we use READSEGMENTS?
                    mozilla::ipc::kDefaultDataPipeCapacity, nullptr, nullptr);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    mBiResolver(rv);
    return rv;
  }

  RefPtr<mozilla::ipc::DataPipeSender> outputsender;
  RefPtr<mozilla::ipc::DataPipeReceiver> outputreceiver;
  rv =
      NewDataPipe(mozilla::ipc::kDefaultDataPipeCapacity,
                  getter_AddRefs(outputsender), getter_AddRefs(outputreceiver));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    mBiResolver(rv);
    return rv;
  }

  nsCOMPtr<nsIAsyncOutputStream> outputStream;
  aStream->GetOutputStream(getter_AddRefs(outputStream));
  MOZ_ASSERT(outputStream);
  rv = NS_AsyncCopy(outputreceiver, outputStream, mSocketThread,
                    NS_ASYNCCOPY_VIA_READSEGMENTS,
                    mozilla::ipc::kDefaultDataPipeCapacity, nullptr, nullptr);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    mBiResolver(rv);
    return rv;
  }

  LOG(("Returning BidirectionalStream pipe to content"));
  mBiResolver(BidirectionalStream(inputreceiver, outputsender));
  return NS_OK;
}

NS_IMETHODIMP
ReceiveStream::OnUnidirectionalStreamReady(nsIWebTransportSendStream* aStream) {
  LOG(("Unidirectional stream ready!"));
  // We should be on the Socket Thread
  MOZ_ASSERT(mSocketThread->IsOnCurrentThread());

  RefPtr<::mozilla::ipc::DataPipeSender> sender;
  RefPtr<::mozilla::ipc::DataPipeReceiver> receiver;
  nsresult rv = NewDataPipe(mozilla::ipc::kDefaultDataPipeCapacity,
                            getter_AddRefs(sender), getter_AddRefs(receiver));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    mUniResolver(nullptr);
    return rv;
  }

  nsCOMPtr<nsIAsyncOutputStream> outputStream;
  aStream->GetOutputStream(getter_AddRefs(outputStream));
  MOZ_ASSERT(outputStream);
  rv = NS_AsyncCopy(receiver, outputStream, mSocketThread,
                    NS_ASYNCCOPY_VIA_READSEGMENTS,
                    mozilla::ipc::kDefaultDataPipeCapacity, nullptr, nullptr);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    mUniResolver(nullptr);
    return rv;
  }

  LOG(("Returning UnidirectionalStream pipe to content"));
  // pass the DataPipeSender to the content process
  mUniResolver(sender);
  return NS_OK;
}

JS_HAZ_CAN_RUN_SCRIPT NS_IMETHODIMP ReceiveStream::OnError(uint8_t aError) {
  nsresult rv = aError == nsIWebTransport::INVALID_STATE_ERROR
                    ? NS_ERROR_DOM_INVALID_STATE_ERR
                    : NS_ERROR_FAILURE;
  if (mUniResolver) {
    mUniResolver(nullptr);
  } else if (mBiResolver) {
    mBiResolver(rv);
  }
  return NS_OK;
}

IPCResult WebTransportParent::RecvCreateUnidirectionalStream(
    Maybe<int64_t> aSendOrder, CreateUnidirectionalStreamResolver&& aResolver) {
  LOG(("%s for %p received, useSendOrder=%d, sendOrder=%" PRIi64, __func__,
       this, aSendOrder.isSome(),
       aSendOrder.isSome() ? aSendOrder.value() : 0));

  RefPtr<ReceiveStream> callback =
      new ReceiveStream(std::move(aResolver), mSocketThread);
  nsresult rv;
  rv = mWebTransport->CreateOutgoingUnidirectionalStream(callback);
  if (NS_FAILED(rv)) {
    callback->OnError(0);  // XXX
  }
  return IPC_OK();
}

IPCResult WebTransportParent::RecvCreateBidirectionalStream(
    Maybe<int64_t> aSendOrder, CreateBidirectionalStreamResolver&& aResolver) {
  LOG(("%s for %p received, useSendOrder=%d, sendOrder=%" PRIi64, __func__,
       this, aSendOrder.isSome(),
       aSendOrder.isSome() ? aSendOrder.value() : 0));

  RefPtr<ReceiveStream> callback =
      new ReceiveStream(std::move(aResolver), mSocketThread);
  nsresult rv;
  rv = mWebTransport->CreateOutgoingBidirectionalStream(callback);
  if (NS_FAILED(rv)) {
    callback->OnError(0);  // XXX
  }
  return IPC_OK();
}

// We recieve this notification from the WebTransportSessionProxy if session was
// successfully created at the end of
// WebTransportSessionProxy::OnStopRequest
NS_IMETHODIMP
WebTransportParent::OnSessionReady(uint64_t aSessionId) {
  MOZ_ASSERT(mOwningEventTarget);
  MOZ_ASSERT(!mOwningEventTarget->IsOnCurrentThread());

  LOG(("Created web transport session, sessionID = %" PRIu64 ", for %p",
       aSessionId, this));

  mSessionReady = true;

  // Retarget to socket thread. After this, WebTransportParent and
  // |mWebTransport| should be only accessed on the socket thread.
  nsresult rv = mWebTransport->RetargetTo(mSocketThread);
  if (NS_FAILED(rv)) {
    mOwningEventTarget->Dispatch(NS_NewRunnableFunction(
        "WebTransportParent::OnSessionReady Failed",
        [self = RefPtr{this}, result = rv] {
          MutexAutoLock lock(self->mMutex);
          if (!self->mClosed && self->mResolver) {
            self->mResolver(ResolveType(
                result, static_cast<uint8_t>(
                            WebTransportReliabilityMode::Supports_unreliable)));
            self->mResolver = nullptr;
          }
        }));
    return NS_OK;
  }

  mOwningEventTarget->Dispatch(NS_NewRunnableFunction(
      "WebTransportParent::OnSessionReady", [self = RefPtr{this}] {
        MutexAutoLock lock(self->mMutex);
        if (!self->mClosed && self->mResolver) {
          self->mResolver(ResolveType(
              NS_OK, static_cast<uint8_t>(
                         WebTransportReliabilityMode::Supports_unreliable)));
          self->mResolver = nullptr;
          if (self->mExecuteAfterResolverCallback) {
            self->mExecuteAfterResolverCallback();
            self->mExecuteAfterResolverCallback = nullptr;
          }
        } else {
          if (self->mClosed) {
            LOG(("Session already closed at OnSessionReady %p", self.get()));
          } else {
            LOG(("No resolver at OnSessionReady %p", self.get()));
          }
        }
      }));

  return NS_OK;
}

// We recieve this notification from the WebTransportSessionProxy if session
// creation was unsuccessful at the end of
// WebTransportSessionProxy::OnStopRequest
NS_IMETHODIMP
WebTransportParent::OnSessionClosed(const uint32_t aErrorCode,
                                    const nsACString& aReason) {
  nsresult rv = NS_OK;

  MOZ_ASSERT(mOwningEventTarget);
  MOZ_ASSERT(!mOwningEventTarget->IsOnCurrentThread());

  // currently we just know if session was closed gracefully or not.
  // we need better error propagation from lower-levels of http3
  // webtransport session and it's subsequent error mapping to DOM.
  // XXX See Bug 1806834
  if (!mSessionReady) {
    LOG(("webtransport %p session creation failed code= %u, reason= %s", this,
         aErrorCode, PromiseFlatCString(aReason).get()));
    // we know we haven't gone Ready yet
    rv = NS_ERROR_FAILURE;
    mOwningEventTarget->Dispatch(NS_NewRunnableFunction(
        "WebTransportParent::OnSessionClosed",
        [self = RefPtr{this}, result = rv] {
          MutexAutoLock lock(self->mMutex);
          if (!self->mClosed && self->mResolver) {
            self->mResolver(ResolveType(
                result, static_cast<uint8_t>(
                            WebTransportReliabilityMode::Supports_unreliable)));
            self->mResolver = nullptr;
          }
        }));
  } else {
    {
      MutexAutoLock lock(mMutex);
      if (mResolver) {
        LOG(("[%p] NotifyRemoteClosed to be called later", this));
        // NotifyRemoteClosed needs to wait until mResolver is invoked.
        mExecuteAfterResolverCallback = [self = RefPtr{this}, aErrorCode,
                                         reason = nsCString{aReason}]() {
          self->NotifyRemoteClosed(aErrorCode, reason);
        };
        return NS_OK;
      }
    }
    // https://w3c.github.io/webtransport/#web-transport-termination
    // Step 1: Let cleanly be a boolean representing whether the HTTP/3
    // stream associated with the CONNECT request that initiated
    // transport.[[Session]] is in the "Data Recvd" state. [QUIC]
    // XXX not calculated yet
    NotifyRemoteClosed(aErrorCode, aReason);
  }

  return NS_OK;
}

void WebTransportParent::NotifyRemoteClosed(uint32_t aErrorCode,
                                            const nsACString& aReason) {
  LOG(("webtransport %p session remote closed code= %u, reason= %s", this,
       aErrorCode, PromiseFlatCString(aReason).get()));
  mSocketThread->Dispatch(NS_NewRunnableFunction(
      __func__,
      [self = RefPtr{this}, aErrorCode, reason = nsCString{aReason}]() {
        // Tell the content side we were closed by the server
        Unused << self->SendRemoteClosed(/*XXX*/ true, aErrorCode, reason);
        // Let the other end shut down the IPC channel after RecvClose()
      }));
}

// This method is currently not used by WebTransportSessionProxy to inform of
// any session related events. All notification is recieved via
// WebTransportSessionProxy::OnSessionReady and
// WebTransportSessionProxy::OnSessionClosed methods
NS_IMETHODIMP
WebTransportParent::OnSessionReadyInternal(
    mozilla::net::Http3WebTransportSession* aSession) {
  Unused << aSession;
  return NS_OK;
}

NS_IMETHODIMP
WebTransportParent::OnIncomingStreamAvailableInternal(
    mozilla::net::Http3WebTransportStream* aStream) {
  // XXX implement once DOM WebAPI supports creation of streams
  Unused << aStream;
  return NS_OK;
}

NS_IMETHODIMP
WebTransportParent::OnIncomingUnidirectionalStreamAvailable(
    nsIWebTransportReceiveStream* aStream) {
  // Note: we need to hold a reference to the stream if we want to get stats,
  // etc
  LOG(("%p IncomingUnidirectonalStream available", this));

  // We must be on the Socket Thread
  MOZ_ASSERT(mSocketThread->IsOnCurrentThread());

  RefPtr<DataPipeSender> sender;
  RefPtr<DataPipeReceiver> receiver;
  nsresult rv = NewDataPipe(mozilla::ipc::kDefaultDataPipeCapacity,
                            getter_AddRefs(sender), getter_AddRefs(receiver));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  nsCOMPtr<nsIAsyncInputStream> inputStream;
  aStream->GetInputStream(getter_AddRefs(inputStream));
  MOZ_ASSERT(inputStream);
  rv = NS_AsyncCopy(inputStream, sender, mSocketThread,
                    NS_ASYNCCOPY_VIA_WRITESEGMENTS,
                    mozilla::ipc::kDefaultDataPipeCapacity);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  LOG(("%p Sending UnidirectionalStream pipe to content", this));
  // pass the DataPipeReceiver to the content process
  Unused << SendIncomingUnidirectionalStream(receiver);

  return NS_OK;
}

NS_IMETHODIMP
WebTransportParent::OnIncomingBidirectionalStreamAvailable(
    nsIWebTransportBidirectionalStream* aStream) {
  // Note: we need to hold a reference to the stream if we want to get stats,
  // etc
  LOG(("%p IncomingBidirectonalStream available", this));

  // We must be on the Socket Thread
  MOZ_ASSERT(mSocketThread->IsOnCurrentThread());

  RefPtr<DataPipeSender> inputSender;
  RefPtr<DataPipeReceiver> inputReceiver;
  nsresult rv =
      NewDataPipe(mozilla::ipc::kDefaultDataPipeCapacity,
                  getter_AddRefs(inputSender), getter_AddRefs(inputReceiver));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  nsCOMPtr<nsIAsyncInputStream> inputStream;
  aStream->GetInputStream(getter_AddRefs(inputStream));
  MOZ_ASSERT(inputStream);
  rv = NS_AsyncCopy(inputStream, inputSender, mSocketThread,
                    NS_ASYNCCOPY_VIA_WRITESEGMENTS,
                    mozilla::ipc::kDefaultDataPipeCapacity);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  RefPtr<DataPipeSender> outputSender;
  RefPtr<DataPipeReceiver> outputReceiver;
  rv =
      NewDataPipe(mozilla::ipc::kDefaultDataPipeCapacity,
                  getter_AddRefs(outputSender), getter_AddRefs(outputReceiver));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  nsCOMPtr<nsIAsyncOutputStream> outputStream;
  aStream->GetOutputStream(getter_AddRefs(outputStream));
  MOZ_ASSERT(outputStream);
  rv = NS_AsyncCopy(outputReceiver, outputStream, mSocketThread,
                    NS_ASYNCCOPY_VIA_READSEGMENTS,
                    mozilla::ipc::kDefaultDataPipeCapacity);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  LOG(("%p Sending BidirectionalStream pipe to content", this));
  // pass the DataPipeSender to the content process
  Unused << SendIncomingBidirectionalStream(inputReceiver, outputSender);
  return NS_OK;
}

// The promise sent in this request will be resolved
// in OnOutgoingDatagramOutCome which is called synchronously from
// WebTransportSessionProxy::SendDatagram
::mozilla::ipc::IPCResult WebTransportParent::RecvOutgoingDatagram(
    nsTArray<uint8_t>&& aData, const TimeStamp& aExpirationTime,
    OutgoingDatagramResolver&& aResolver) {
  LOG(("WebTransportParent sending datagram"));
  MOZ_ASSERT(mSocketThread->IsOnCurrentThread());
  MOZ_ASSERT(!mOutgoingDatagramResolver);
  MOZ_ASSERT(mWebTransport);

  Unused << aExpirationTime;

  MOZ_ASSERT(!mOutgoingDatagramResolver);
  mOutgoingDatagramResolver = std::move(aResolver);
  // XXX we need to forward the timestamps to the necko stack
  // timestamp should be checked in the necko for expiry
  // See Bug 1818300
  // currently this calls OnOutgoingDatagramOutCome synchronously
  // Neqo won't call us back if the id == 0!
  // We don't use the ID for anything currently; rework of the stack
  // to implement proper HighWatermark buffering will require
  // changes here anyways.
  static uint64_t sDatagramId = 1;
  LOG_VERBOSE(("Sending datagram %" PRIu64 ", length %zu", sDatagramId,
               aData.Length()));
  Unused << mWebTransport->SendDatagram(aData, sDatagramId++);

  return IPC_OK();
}

NS_IMETHODIMP WebTransportParent::OnDatagramReceived(
    const nsTArray<uint8_t>& aData) {
  // We must be on the Socket Thread
  MOZ_ASSERT(mSocketThread->IsOnCurrentThread());

  LOG(("WebTransportParent received datagram length = %zu", aData.Length()));

  TimeStamp ts = TimeStamp::Now();
  Unused << SendIncomingDatagram(aData, ts);

  return NS_OK;
}

NS_IMETHODIMP WebTransportParent::OnDatagramReceivedInternal(
    nsTArray<uint8_t>&& aData) {
  // this method is used only for internal notificaiton within necko
  // we dont expect to receive any notification with on this interface
  return NS_OK;
}

NS_IMETHODIMP
WebTransportParent::OnOutgoingDatagramOutCome(
    uint64_t aId, WebTransportSessionEventListener::DatagramOutcome aOutCome) {
  MOZ_ASSERT(mSocketThread->IsOnCurrentThread());
  // XXX - do we need better error mappings for failures?
  nsresult result = NS_ERROR_FAILURE;
  Unused << result;
  Unused << aId;

  if (aOutCome == WebTransportSessionEventListener::DatagramOutcome::SENT) {
    result = NS_OK;
    LOG(("Sent datagram id= %" PRIu64, aId));
  } else {
    LOG(("Didn't send datagram id= %" PRIu64, aId));
  }

  // This assumes the stack is calling us back synchronously!
  MOZ_ASSERT(mOutgoingDatagramResolver);
  mOutgoingDatagramResolver(result);
  mOutgoingDatagramResolver = nullptr;

  return NS_OK;
}

NS_IMETHODIMP WebTransportParent::OnMaxDatagramSize(uint64_t aSize) {
  // XXX -  we need to pass this to  WebTransportChild via IPC
  // See Bug 1818763
  return NS_OK;
}
}  // namespace mozilla::dom
