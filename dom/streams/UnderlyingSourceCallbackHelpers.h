/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim:set ts=2 sw=2 sts=2 et cindent: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_UnderlyingSourceCallbackHelpers_h
#define mozilla_dom_UnderlyingSourceCallbackHelpers_h

#include "mozilla/HoldDropJSObjects.h"
#include "mozilla/dom/Promise.h"
#include "mozilla/dom/UnderlyingSourceBinding.h"
#include "nsIAsyncInputStream.h"
#include "nsISupports.h"
#include "nsISupportsImpl.h"

/* Since the streams specification has native descriptions of some callbacks
 * (i.e. described in prose, rather than provided by user code), we need to be
 * able to pass around native callbacks. To handle this, we define polymorphic
 * classes That cover the difference between native callback and user-provided.
 *
 * The Streams specification wants us to invoke these callbacks, run through
 * WebIDL as if they were methods. So we have to preserve the underlying object
 * to use as the This value on invocation.
 */
enum class nsresult : uint32_t;

namespace mozilla::dom {

class StrongWorkerRef;
class BodyStreamHolder;
class ReadableStreamController;
class ReadableStream;

class UnderlyingSourceAlgorithmsBase : public nsISupports {
 public:
  NS_DECL_CYCLE_COLLECTING_ISUPPORTS
  NS_DECL_CYCLE_COLLECTION_CLASS(UnderlyingSourceAlgorithmsBase)

  MOZ_CAN_RUN_SCRIPT virtual void StartCallback(
      JSContext* aCx, ReadableStreamController& aController,
      JS::MutableHandle<JS::Value> aRetVal, ErrorResult& aRv) = 0;

  // A promise-returning algorithm that pulls data from the underlying byte
  // source
  MOZ_CAN_RUN_SCRIPT virtual already_AddRefed<Promise> PullCallback(
      JSContext* aCx, ReadableStreamController& aController,
      ErrorResult& aRv) = 0;

  // A promise-returning algorithm, taking one argument (the cancel reason),
  // which communicates a requested cancelation to the underlying byte source
  MOZ_CAN_RUN_SCRIPT virtual already_AddRefed<Promise> CancelCallback(
      JSContext* aCx, const Optional<JS::Handle<JS::Value>>& aReason,
      ErrorResult& aRv) = 0;

  // Implement this when you need to release underlying resources immediately
  // from closed(canceled)/errored streams, without waiting for GC.
  virtual void ReleaseObjects() {}

  // Fetch wants to special-case BodyStream-based streams
  virtual BodyStreamHolder* GetBodyStreamHolder() { return nullptr; }

  // https://streams.spec.whatwg.org/#other-specs-rs-create
  // By "native" we mean "instances initialized via the above set up or set up
  // with byte reading support algorithms (not, e.g., on web-developer-created
  // instances)"
  virtual bool IsNative() { return true; }

 protected:
  virtual ~UnderlyingSourceAlgorithmsBase() = default;
};

class UnderlyingSourceAlgorithms final : public UnderlyingSourceAlgorithmsBase {
 public:
  NS_DECL_ISUPPORTS_INHERITED
  NS_DECL_CYCLE_COLLECTION_SCRIPT_HOLDER_CLASS_INHERITED(
      UnderlyingSourceAlgorithms, UnderlyingSourceAlgorithmsBase)

  UnderlyingSourceAlgorithms(nsIGlobalObject* aGlobal,
                             JS::Handle<JSObject*> aUnderlyingSource,
                             UnderlyingSource& aUnderlyingSourceDict)
      : mGlobal(aGlobal), mUnderlyingSource(aUnderlyingSource) {
    // Step 6. (implicit Step 2.)
    if (aUnderlyingSourceDict.mStart.WasPassed()) {
      mStartCallback = aUnderlyingSourceDict.mStart.Value();
    }

    // Step 7. (implicit Step 3.)
    if (aUnderlyingSourceDict.mPull.WasPassed()) {
      mPullCallback = aUnderlyingSourceDict.mPull.Value();
    }

    // Step 8. (implicit Step 4.)
    if (aUnderlyingSourceDict.mCancel.WasPassed()) {
      mCancelCallback = aUnderlyingSourceDict.mCancel.Value();
    }

    mozilla::HoldJSObjects(this);
  };

  MOZ_CAN_RUN_SCRIPT void StartCallback(JSContext* aCx,
                                        ReadableStreamController& aController,
                                        JS::MutableHandle<JS::Value> aRetVal,
                                        ErrorResult& aRv) override;

  MOZ_CAN_RUN_SCRIPT already_AddRefed<Promise> PullCallback(
      JSContext* aCx, ReadableStreamController& aController,
      ErrorResult& aRv) override;

  MOZ_CAN_RUN_SCRIPT already_AddRefed<Promise> CancelCallback(
      JSContext* aCx, const Optional<JS::Handle<JS::Value>>& aReason,
      ErrorResult& aRv) override;

  bool IsNative() override { return false; }

 protected:
  ~UnderlyingSourceAlgorithms() override { mozilla::DropJSObjects(this); };

 private:
  // Virtually const, but are cycle collected
  nsCOMPtr<nsIGlobalObject> mGlobal;
  JS::Heap<JSObject*> mUnderlyingSource;
  MOZ_KNOWN_LIVE RefPtr<UnderlyingSourceStartCallback> mStartCallback;
  MOZ_KNOWN_LIVE RefPtr<UnderlyingSourcePullCallback> mPullCallback;
  MOZ_KNOWN_LIVE RefPtr<UnderlyingSourceCancelCallback> mCancelCallback;
};

// https://streams.spec.whatwg.org/#readablestream-set-up
// https://streams.spec.whatwg.org/#readablestream-set-up-with-byte-reading-support
// Wrappers defined by the "Set up" methods in the spec. This helps you just
// return nullptr when an error occurred as this wrapper converts it to a
// rejected promise.
// Note that StartCallback is only for JS consumers to access
// the controller, and thus is no-op here since native consumers can call
// `EnqueueNative()` etc. without direct controller access.
class UnderlyingSourceAlgorithmsWrapper
    : public UnderlyingSourceAlgorithmsBase {
  void StartCallback(JSContext*, ReadableStreamController&,
                     JS::MutableHandle<JS::Value> aRetVal, ErrorResult&) final;

  MOZ_CAN_RUN_SCRIPT already_AddRefed<Promise> PullCallback(
      JSContext* aCx, ReadableStreamController& aController,
      ErrorResult& aRv) final;

  MOZ_CAN_RUN_SCRIPT already_AddRefed<Promise> CancelCallback(
      JSContext* aCx, const Optional<JS::Handle<JS::Value>>& aReason,
      ErrorResult& aRv) final;

  MOZ_CAN_RUN_SCRIPT virtual already_AddRefed<Promise> PullCallbackImpl(
      JSContext* aCx, ReadableStreamController& aController, ErrorResult& aRv) {
    // pullAlgorithm is optional, return null by default
    return nullptr;
  }

  virtual already_AddRefed<Promise> CancelCallbackImpl(
      JSContext* aCx, const Optional<JS::Handle<JS::Value>>& aReason,
      ErrorResult& aRv) {
    // cancelAlgorithm is optional, return null by default
    return nullptr;
  }
};

class InputToReadableStreamAlgorithms;

// This class exists to isolate InputToReadableStreamAlgorithms from the
// nsIAsyncInputStream.  If we call AsyncWait(this,...), it holds a
// reference to 'this' which can't be cc'd, and we can leak the stream,
// causing a Worker to assert with globalScopeAlive.  By isolating
// ourselves from the inputstream, we can safely be CC'd if needed and
// will inform the inputstream to shut down.
class InputStreamHolder final : public nsIInputStreamCallback {
 public:
  NS_DECL_THREADSAFE_ISUPPORTS
  NS_DECL_NSIINPUTSTREAMCALLBACK

  InputStreamHolder(JSContext* aCx, InputToReadableStreamAlgorithms* aCallback,
                    nsIAsyncInputStream* aInput);

  // This MUST be called before we're destroyed
  void Shutdown();

  // These just proxy the calls to the nsIAsyncInputStream
  nsresult AsyncWait(uint32_t aFlags, uint32_t aRequestedCount,
                     nsIEventTarget* aEventTarget);
  nsresult Available(uint64_t* aSize) { return mInput->Available(aSize); }
  nsresult Read(char* aBuffer, uint32_t aLength, uint32_t* aWritten) {
    return mInput->Read(aBuffer, aLength, aWritten);
  }
  nsresult CloseWithStatus(nsresult aStatus) {
    return mInput->CloseWithStatus(aStatus);
  }

 private:
  ~InputStreamHolder();

  // Purposely rawptr to avoid cycles.  Must be cleared with Shutdown() before
  // destruction
  InputToReadableStreamAlgorithms* mCallback;
  // To ensure the worker sticks around
  RefPtr<StrongWorkerRef> mAsyncWaitWorkerRef;
  RefPtr<StrongWorkerRef> mWorkerRef;
  nsCOMPtr<nsIAsyncInputStream> mInput;
};

class InputToReadableStreamAlgorithms final
    : public UnderlyingSourceAlgorithmsWrapper,
      public nsIInputStreamCallback {
  NS_DECL_ISUPPORTS_INHERITED
  NS_DECL_NSIINPUTSTREAMCALLBACK
  NS_DECL_CYCLE_COLLECTION_CLASS_INHERITED(InputToReadableStreamAlgorithms,
                                           UnderlyingSourceAlgorithmsWrapper)

  InputToReadableStreamAlgorithms(JSContext* aCx, nsIAsyncInputStream* aInput,
                                  ReadableStream* aStream)
      : mState(eInitializing),
        mOwningEventTarget(GetCurrentSerialEventTarget()),
        mInput(new InputStreamHolder(aCx, this, aInput)),
        mStream(aStream) {}

  // Streams algorithms

  already_AddRefed<Promise> PullCallbackImpl(
      JSContext* aCx, ReadableStreamController& aController,
      ErrorResult& aRv) override;

  void ReleaseObjects() override;

 private:
  ~InputToReadableStreamAlgorithms() {
    if (mInput) {
      mInput->Shutdown();
    }
  }

  MOZ_CAN_RUN_SCRIPT_BOUNDARY void CloseAndReleaseObjects(
      JSContext* aCx, ReadableStream* aStream);

  void WriteIntoReadRequestBuffer(JSContext* aCx, ReadableStream* aStream,
                                  JS::Handle<JSObject*> aBuffer,
                                  uint32_t aLength, uint32_t* aByteWritten);

  MOZ_CAN_RUN_SCRIPT_BOUNDARY void EnqueueChunkWithSizeIntoStream(
      JSContext* aCx, ReadableStream* aStream, uint64_t aAvailableData,
      ErrorResult& aRv);
  void ErrorPropagation(JSContext* aCx, ReadableStream* aStream,
                        nsresult aError);

  // Common methods

  enum State {
    // This is the beginning state before any reading operation.
    eInitializing,

    // RequestDataCallback has not been called yet. We haven't started to read
    // data from the stream yet.
    eWaiting,

    // We are reading data in a separate I/O thread.
    eReading,

    // We are ready to write something in the JS Buffer.
    eWriting,

    // After a writing, we want to check if the stream is closed. After the
    // check, we go back to eWaiting. If a reading request happens in the
    // meantime, we move to eReading state.
    eChecking,

    // Operation completed.
    eClosed,
  };

  State mState;

  nsCOMPtr<nsIEventTarget> mOwningEventTarget;

  RefPtr<InputStreamHolder> mInput;
  RefPtr<ReadableStream> mStream;
};

}  // namespace mozilla::dom

#endif
