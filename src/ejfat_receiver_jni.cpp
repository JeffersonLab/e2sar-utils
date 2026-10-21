/*
 * Copyright (c) 2025, Jefferson Science Associates, all rights reserved.
 * See LICENSE.txt file.
 *
 * JNI bridge that exposes the E2SAR Reassembler to Java. Mirrors the
 * reassembler setup used by ejfat_receiver_actor.cpp, but is compiled as a
 * separate shared library (libEjfatReceiverJni.so) so the ERSAP C++ plugin
 * remains free of JVM linkage.
 *
 * Consumed by:
 *   org.jlab.epsci.ersap.examples.engines.generic.EjfatReceiverSource
 *
 * Protocol (see EjfatReceiverSource.java for the Java side):
 *   nativeCreate(...)  -> constructs Reassembler, registers worker (if CP),
 *                        calls openAndStart(), returns opaque handle
 *   nativePoll(h, ms)  -> polls Reassembler.recvEvent in POLL_STEP_MS slices
 *                        up to ms; returns pending envelope size or -1 on
 *                        timeout. Native side retains the reassembled buffer
 *                        until nativeConsume runs.
 *   nativeConsume(h,b) -> writes [data_id as double][body] into direct
 *                        ByteBuffer b (exactly one memcpy of the body),
 *                        releases the E2SAR-allocated buffer.
 *   nativeLastEventNum -> event_num of the last consumed event
 *   nativeLastDataId   -> data_id of the last consumed event
 *   nativeDestroy(h)   -> deregisters worker, stops threads, frees the handle
 *
 * Thread safety: the Java caller (AbstractEventReaderService) serializes
 * poll+consume through its reader lock, so the bridge does not need to guard
 * pending-buffer state against concurrent poll calls. A mutex still guards
 * destroy vs. poll to keep shutdown race-free.
 */

#include <jni.h>

#include <e2sar.hpp>
#include <boost/asio.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <string>

namespace {

constexpr int POLL_STEP_MS = 100;
constexpr std::size_t HEADER_SIZE = sizeof(double);  // data_id encoded as double

// -----------------------------------------------------------------------
// Bridge state
// -----------------------------------------------------------------------

struct EjfatBridge {
    std::unique_ptr<e2sar::Reassembler> reassembler;
    std::mutex lifecycle_mutex;   // guards destroy vs. in-flight poll
    std::atomic<bool> shutting_down{false};
    bool worker_registered = false;
    int max_wait_ms = 5000;

    // Pending payload (set by nativePoll, drained by nativeConsume).
    // The buffer pointer is owned by E2SAR; we release it with delete[].
    std::uint8_t* pending_body = nullptr;
    std::size_t pending_body_size = 0;
    e2sar::EventNum_t last_event_num = 0;
    std::uint16_t last_data_id = 0;

    ~EjfatBridge() {
        if (pending_body != nullptr) {
            delete[] pending_body;
            pending_body = nullptr;
        }
    }
};

inline EjfatBridge* toBridge(jlong handle) {
    return reinterpret_cast<EjfatBridge*>(static_cast<std::intptr_t>(handle));
}

inline jlong toHandle(EjfatBridge* bridge) {
    return static_cast<jlong>(reinterpret_cast<std::intptr_t>(bridge));
}

// -----------------------------------------------------------------------
// Exception helpers
// -----------------------------------------------------------------------

void throwJava(JNIEnv* env, const char* className, const std::string& msg) {
    jclass cls = env->FindClass(className);
    if (cls != nullptr) {
        env->ThrowNew(cls, msg.c_str());
        env->DeleteLocalRef(cls);
    }
}

void throwIO(JNIEnv* env, const std::string& msg) {
    throwJava(env, "java/io/IOException", msg);
}

void throwIllegalArg(JNIEnv* env, const std::string& msg) {
    throwJava(env, "java/lang/IllegalArgumentException", msg);
}

void throwIllegalState(JNIEnv* env, const std::string& msg) {
    throwJava(env, "java/lang/IllegalStateException", msg);
}

// -----------------------------------------------------------------------
// jstring -> std::string helper (null-safe)
// -----------------------------------------------------------------------

std::string toStdString(JNIEnv* env, jstring js) {
    if (js == nullptr) {
        return {};
    }
    const char* raw = env->GetStringUTFChars(js, nullptr);
    if (raw == nullptr) {
        return {};
    }
    std::string result(raw);
    env->ReleaseStringUTFChars(js, raw);
    return result;
}

}  // namespace

// ===========================================================================
// JNI entry points
// ===========================================================================

extern "C" {

JNIEXPORT jlong JNICALL
Java_org_jlab_epsci_ersap_examples_engines_generic_EjfatReceiverSource_nativeCreate(
    JNIEnv* env, jclass /*clazz*/,
    jstring jUri, jstring jRecvIp, jint jRecvPort, jint jRecvThreads,
    jint jEventTimeoutMs, jint jMaxWaitMs,
    jboolean jWithCp, jboolean jValidateCert)
{
    if (jUri == nullptr) {
        throwIllegalArg(env, "ejfat_uri is null");
        return 0;
    }
    if (jRecvPort <= 0 || jRecvPort > 0xFFFF) {
        throwIllegalArg(env, "recv_port out of range");
        return 0;
    }
    if (jRecvThreads <= 0) {
        throwIllegalArg(env, "recv_threads must be > 0");
        return 0;
    }
    if (jEventTimeoutMs <= 0) {
        throwIllegalArg(env, "event_timeout_ms must be > 0");
        return 0;
    }
    if (jMaxWaitMs <= 0) {
        throwIllegalArg(env, "max_wait_ms must be > 0");
        return 0;
    }

    std::string uri    = toStdString(env, jUri);
    std::string recvIp = (jRecvIp != nullptr) ? toStdString(env, jRecvIp) : "0.0.0.0";
    if (recvIp.empty()) {
        recvIp = "0.0.0.0";
    }

    auto uri_result = e2sar::EjfatURI::getFromString(
        uri, e2sar::EjfatURI::TokenType::instance, false);
    if (uri_result.has_error()) {
        throwIO(env, std::string("failed to parse EJFAT URI: ")
                     + uri_result.error().message());
        return 0;
    }

    boost::asio::ip::address ip;
    try {
        ip = boost::asio::ip::make_address(recvIp);
    } catch (const std::exception& e) {
        throwIllegalArg(env, std::string("invalid recv_ip: ") + e.what());
        return 0;
    }

    e2sar::Reassembler::ReassemblerFlags rflags;
    rflags.useCP           = (jWithCp == JNI_TRUE);
    // Same rule as the C++ ERSAP actor: when the control plane is off, the
    // segmenter's LB header stays on the wire and the reassembler must expect it.
    rflags.withLBHeader    = (jWithCp == JNI_FALSE);
    rflags.eventTimeout_ms = static_cast<int>(jEventTimeoutMs);
    if (jWithCp == JNI_TRUE) {
        rflags.validateCert = (jValidateCert == JNI_TRUE);
    }

    auto bridge = std::make_unique<EjfatBridge>();
    bridge->max_wait_ms = static_cast<int>(jMaxWaitMs);

    try {
        bridge->reassembler = std::make_unique<e2sar::Reassembler>(
            uri_result.value(), ip,
            static_cast<std::uint16_t>(jRecvPort),
            static_cast<std::size_t>(jRecvThreads),
            rflags);
    } catch (const std::exception& e) {
        throwIO(env, std::string("failed to construct Reassembler: ") + e.what());
        return 0;
    }

    auto hostname_res = e2sar::NetUtil::getHostName();
    if (!hostname_res.has_error()) {
        auto regres = bridge->reassembler->registerWorker(hostname_res.value());
        if (regres.has_error()) {
            // Registration is a no-op without the control plane, and best-effort otherwise.
            // Do not fail creation; the receive path can still work.
        } else {
            bridge->worker_registered = true;
        }
    }

    auto open_result = bridge->reassembler->openAndStart();
    if (open_result.has_error()) {
        throwIO(env, std::string("failed to start Reassembler: ")
                     + open_result.error().message());
        return 0;
    }

    return toHandle(bridge.release());
}

JNIEXPORT jint JNICALL
Java_org_jlab_epsci_ersap_examples_engines_generic_EjfatReceiverSource_nativePoll(
    JNIEnv* env, jclass /*clazz*/, jlong handle, jint jMaxWaitMs)
{
    EjfatBridge* bridge = toBridge(handle);
    if (bridge == nullptr || bridge->reassembler == nullptr) {
        throwIllegalState(env, "EJFAT bridge handle is invalid");
        return -1;
    }

    // A prior poll that was never consumed would leak the E2SAR-allocated
    // buffer; enforce the poll-then-consume contract.
    if (bridge->pending_body != nullptr) {
        throwIllegalState(env,
            "EJFAT bridge: pending event has not been consumed; "
            "call nativeConsume before the next nativePoll");
        return -1;
    }

    std::uint8_t* event_buffer = nullptr;
    std::size_t event_size = 0;
    e2sar::EventNum_t event_num = 0;
    std::uint16_t data_id = 0;

    const int max_wait_ms = (jMaxWaitMs > 0) ? static_cast<int>(jMaxWaitMs)
                                             : bridge->max_wait_ms;
    const auto deadline = std::chrono::steady_clock::now()
                        + std::chrono::milliseconds(max_wait_ms);

    while (!bridge->shutting_down.load()) {
        auto result = bridge->reassembler->recvEvent(
            &event_buffer, &event_size, &event_num, &data_id, POLL_STEP_MS);

        if (result.has_error()) {
            if (std::chrono::steady_clock::now() >= deadline) {
                return -1;
            }
            continue;
        }
        if (result.value() == -1) {
            if (std::chrono::steady_clock::now() >= deadline) {
                return -1;
            }
            continue;
        }

        bridge->pending_body      = event_buffer;
        bridge->pending_body_size = event_size;
        bridge->last_event_num    = event_num;
        bridge->last_data_id      = data_id;

        const std::size_t envelope = HEADER_SIZE + event_size;
        if (envelope > static_cast<std::size_t>(std::numeric_limits<jint>::max())) {
            // Guard against pathological events that don't fit in a jint.
            delete[] bridge->pending_body;
            bridge->pending_body = nullptr;
            bridge->pending_body_size = 0;
            throwIO(env, "reassembled event is too large to expose to Java");
            return -1;
        }
        return static_cast<jint>(envelope);
    }
    return -1;
}

JNIEXPORT void JNICALL
Java_org_jlab_epsci_ersap_examples_engines_generic_EjfatReceiverSource_nativeConsume(
    JNIEnv* env, jclass /*clazz*/, jlong handle, jobject dst)
{
    EjfatBridge* bridge = toBridge(handle);
    if (bridge == nullptr) {
        throwIllegalState(env, "EJFAT bridge handle is invalid");
        return;
    }
    if (dst == nullptr) {
        throwIllegalArg(env, "destination ByteBuffer is null");
        return;
    }

    void* addr = env->GetDirectBufferAddress(dst);
    if (addr == nullptr) {
        throwIllegalArg(env,
            "destination ByteBuffer is not direct (allocateDirect required)");
        return;
    }
    jlong capacity = env->GetDirectBufferCapacity(dst);
    if (capacity < 0) {
        throwIllegalArg(env, "destination ByteBuffer has unknown capacity");
        return;
    }

    const std::size_t envelope = HEADER_SIZE + bridge->pending_body_size;
    if (static_cast<std::size_t>(capacity) < envelope) {
        throwIllegalArg(env,
            "destination ByteBuffer is too small for pending event");
        return;
    }

    // Envelope layout: [data_id as double][reassembled body]
    auto* out = static_cast<std::uint8_t*>(addr);
    const double data_id_as_double = static_cast<double>(bridge->last_data_id);
    std::memcpy(out, &data_id_as_double, HEADER_SIZE);
    if (bridge->pending_body_size > 0 && bridge->pending_body != nullptr) {
        std::memcpy(out + HEADER_SIZE, bridge->pending_body, bridge->pending_body_size);
    }

    // Release the E2SAR-owned buffer and clear the pending slot.
    delete[] bridge->pending_body;
    bridge->pending_body = nullptr;
    bridge->pending_body_size = 0;
}

JNIEXPORT jlong JNICALL
Java_org_jlab_epsci_ersap_examples_engines_generic_EjfatReceiverSource_nativeLastEventNum(
    JNIEnv* env, jclass /*clazz*/, jlong handle)
{
    EjfatBridge* bridge = toBridge(handle);
    if (bridge == nullptr) {
        throwIllegalState(env, "EJFAT bridge handle is invalid");
        return -1;
    }
    return static_cast<jlong>(bridge->last_event_num);
}

JNIEXPORT jint JNICALL
Java_org_jlab_epsci_ersap_examples_engines_generic_EjfatReceiverSource_nativeLastDataId(
    JNIEnv* env, jclass /*clazz*/, jlong handle)
{
    EjfatBridge* bridge = toBridge(handle);
    if (bridge == nullptr) {
        throwIllegalState(env, "EJFAT bridge handle is invalid");
        return -1;
    }
    return static_cast<jint>(bridge->last_data_id);
}

JNIEXPORT void JNICALL
Java_org_jlab_epsci_ersap_examples_engines_generic_EjfatReceiverSource_nativeDestroy(
    JNIEnv* /*env*/, jclass /*clazz*/, jlong handle)
{
    EjfatBridge* bridge = toBridge(handle);
    if (bridge == nullptr) {
        return;
    }

    bridge->shutting_down.store(true);

    {
        std::lock_guard<std::mutex> lock(bridge->lifecycle_mutex);
        if (bridge->reassembler) {
            if (bridge->worker_registered) {
                (void) bridge->reassembler->deregisterWorker();
                bridge->worker_registered = false;
            }
            bridge->reassembler->stopThreads();
            bridge->reassembler.reset();
        }
    }

    delete bridge;
}

}  // extern "C"
