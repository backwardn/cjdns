/* vim: set expandtab ts=4 sw=4: */
/*
 * You may redistribute this program and/or modify it under the terms of
 * the GNU General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */
#include "memory/Allocator.h"
#include "wire/PFChan.h"
#include "net/SessionManager.h"
#include "crypto/AddressCalc.h"
#include "util/AddrTools.h"
#include "wire/Error.h"
#include "util/events/Time.h"
#include "util/Defined.h"
#include "wire/RouteHeader.h"
#include "util/events/Timeout.h"
#include "util/Checksum.h"
#include "wire/Metric.h"

/** Handle numbers 0-3 are reserved for CryptoAuth nonces. */
#define MIN_FIRST_HANDLE 4

#define MAX_FIRST_HANDLE 100000

struct BufferedMessage
{
    struct Message* msg;
    struct Allocator* alloc;
    uint64_t timeSentMilliseconds;
};

struct Ip6 {
    uint8_t bytes[16];
};
#define Map_KEY_TYPE struct Ip6
#define Map_VALUE_TYPE struct BufferedMessage*
#define Map_NAME BufferedMessages
#include "util/Map.h"

#define Map_KEY_TYPE struct Ip6
#define Map_VALUE_TYPE struct SessionManager_Session_pvt*
#define Map_NAME OfSessionsByIp6
#define Map_ENABLE_HANDLES
#include "util/Map.h"

struct SessionManager_pvt
{
    struct SessionManager pub;
    struct Iface eventIf;
    struct Allocator* alloc;
    struct Map_BufferedMessages bufMap;
    struct Map_OfSessionsByIp6 ifaceMap;
    struct Log* log;
    struct CryptoAuth* cryptoAuth;
    struct EventBase* eventBase;
    uint32_t firstHandle;
    Identity
};

struct SessionManager_Session_pvt
{
    struct SessionManager_Session pub;

    struct SessionManager_pvt* sessionManager;

    struct Allocator* alloc;

    bool foundKey;

    Identity
};

#define debugHandlesAndLabel(logger, session, label, message, ...) \
    do {                                                                               \
        if (!Defined(Log_DEBUG)) { break; }                                            \
        uint8_t path[20];                                                              \
        AddrTools_printPath(path, label);                                              \
        uint8_t ip[40];                                                                \
        AddrTools_printIp(ip, session->pub.caSession->herIp6);                         \
        Log_debug(logger, "ver[%u] send[%d] recv[%u] ip[%s] path[%s] " message,        \
                  session->pub.version,                                                \
                  session->pub.sendHandle,                                             \
                  session->pub.receiveHandle,                                          \
                  ip,                                                                  \
                  path,                                                                \
                  __VA_ARGS__);                                                        \
    } while (0)
//CHECKFILES_IGNORE expecting a ;

#define debugHandlesAndLabel0(logger, session, label, message) \
    debugHandlesAndLabel(logger, session, label, "%s", message)

#define debugSession(logger, session, message, ...) \
    do {                                                                               \
        if (!Defined(Log_DEBUG)) { break; }                                            \
        uint8_t sendPath[20];                                                          \
        uint8_t recvPath[20];                                                          \
        uint8_t ip[40];                                                                \
        AddrTools_printPath(sendPath, (session)->pub.sendSwitchLabel);                 \
        AddrTools_printPath(recvPath, (session)->pub.recvSwitchLabel);                 \
        AddrTools_printIp(ip, (session)->pub.caSession->herIp6);                       \
        Log_debug((logger), "Session[%p] sendPath[%s] recvPath[%s] ip[%s] " message,   \
                  (void*)session,                                                      \
                  sendPath,                                                            \
                  recvPath,                                                            \
                  ip,                                                                  \
                  __VA_ARGS__);                                                        \
    } while (0)
//CHECKFILES_IGNORE ;

#define debugSession0(logger, session, message) \
    debugSession(logger, session, "%s", message)

static void sendSession(struct SessionManager_Session_pvt* sess,
                        uint64_t path,
                        uint32_t destPf,
                        enum PFChan_Core ev)
{
    struct PFChan_Node session = {
        .path_be = Endian_hostToBigEndian64(path),
        .metric_be = Endian_bigEndianToHost32(sess->pub.metric),
        .version_be = Endian_hostToBigEndian32(sess->pub.version)
    };
    Bits_memcpy(session.ip6, sess->pub.caSession->herIp6, 16);
    Bits_memcpy(session.publicKey, sess->pub.caSession->herPublicKey, 32);

    struct Allocator* alloc = Allocator_child(sess->alloc);
    struct Message* msg = Message_new(0, PFChan_Node_SIZE + 512, alloc);
    Er_assert(Message_epush(msg, &session, PFChan_Node_SIZE));
    Er_assert(Message_epush32be(msg, destPf));
    Er_assert(Message_epush32be(msg, ev));
    Iface_send(&sess->sessionManager->eventIf, msg);
    Allocator_free(alloc);
}

static inline void check(struct SessionManager_pvt* sm, int mapIndex)
{
    struct SessionManager_Session_pvt* ssp = Identity_check(sm->ifaceMap.values[mapIndex]);
    if (ssp->foundKey) { return; }
    uint8_t* herPubKey = ssp->pub.caSession->herPublicKey;
    if (!Bits_isZero(herPubKey, 32)) {
        uint8_t ip6[16];
        AddressCalc_addressForPublicKey(ip6, herPubKey);
        Assert_true(!Bits_memcmp(&sm->ifaceMap.keys[mapIndex], ip6, 16));
        ssp->foundKey = true;
    }
}

static inline struct SessionManager_Session_pvt* sessionForHandle(uint32_t handle,
                                                                  struct SessionManager_pvt* sm)
{
    int index = Map_OfSessionsByIp6_indexForHandle(handle - sm->firstHandle, &sm->ifaceMap);
    if (index < 0) { return NULL; }
    check(sm, index);
    return Identity_check(sm->ifaceMap.values[index]);
}

struct SessionManager_Session* SessionManager_sessionForHandle(uint32_t handle,
                                                               struct SessionManager* manager)
{
    struct SessionManager_pvt* sm = Identity_check((struct SessionManager_pvt*) manager);
    return (struct SessionManager_Session*) sessionForHandle(handle, sm);
}

static inline struct SessionManager_Session_pvt* sessionForIp6(uint8_t ip6[16],
                                                               struct SessionManager_pvt* sm)
{
    int ifaceIndex = Map_OfSessionsByIp6_indexForKey((struct Ip6*)ip6, &sm->ifaceMap);
    if (ifaceIndex == -1) { return NULL; }
    check(sm, ifaceIndex);
    return Identity_check(sm->ifaceMap.values[ifaceIndex]);
}

struct SessionManager_Session* SessionManager_sessionForIp6(uint8_t* ip6,
                                                            struct SessionManager* manager)
{
    struct SessionManager_pvt* sm = Identity_check((struct SessionManager_pvt*) manager);
    return (struct SessionManager_Session*) sessionForIp6(ip6, sm);
}

struct SessionManager_HandleList* SessionManager_getHandleList(struct SessionManager* manager,
                                                               struct Allocator* alloc)
{
    struct SessionManager_pvt* sm = Identity_check((struct SessionManager_pvt*) manager);
    struct SessionManager_HandleList* out =
        Allocator_calloc(alloc, sizeof(struct SessionManager_HandleList), 1);
    uint32_t* buff = Allocator_calloc(alloc, 4, sm->ifaceMap.count);
    out->length = sm->ifaceMap.count;
    out->handles = buff;
    for (int i = 0; i < out->length; i++) {
        buff[i] = sm->ifaceMap.handles[i] + sm->firstHandle;
    }
    return out;
}

static struct SessionManager_Session_pvt* getSession(struct SessionManager_pvt* sm,
                                                     uint8_t ip6[16],
                                                     uint8_t pubKey[32],
                                                     uint32_t version,
                                                     uint64_t label,
                                                     uint32_t metric,
                                                     int maintainSession)
{
    Assert_true(AddressCalc_validAddress(ip6));
    struct SessionManager_Session_pvt* sess = sessionForIp6(ip6, sm);
    if (sess) {
        sess->pub.version = (sess->pub.version) ? sess->pub.version : version;
        sess->pub.maintainSession |= maintainSession;
        if (metric == Metric_DEAD_LINK) {
            // this is a broken path
            if (sess->pub.sendSwitchLabel == label) {
                debugSession0(sm->log, sess, "broken path");
                if (sess->pub.sendSwitchLabel == sess->pub.recvSwitchLabel) {
                    sess->pub.sendSwitchLabel = 0;
                    sess->pub.metric = Metric_DEAD_LINK;
                } else {
                    sess->pub.sendSwitchLabel = sess->pub.recvSwitchLabel;
                    sess->pub.metric = Metric_SM_INCOMING;
                }
            }
        } else if (metric <= sess->pub.metric && label) {
            sess->pub.sendSwitchLabel = label;
            sess->pub.version = (version) ? version : sess->pub.version;
            sess->pub.metric = metric;
            debugSession0(sm->log, sess, "discovered path");
        }
        return sess;
    }
    struct Allocator* alloc = Allocator_child(sm->alloc);
    sess = Allocator_calloc(alloc, sizeof(struct SessionManager_Session_pvt), 1);
    Identity_set(sess);

    sess->pub.caSession = CryptoAuth_newSession(sm->cryptoAuth, alloc, pubKey, false, "inner");

    sess->foundKey = !Bits_isZero(pubKey, 32);
    if (sess->foundKey) {
        uint8_t realIp6[16];
        AddressCalc_addressForPublicKey(realIp6, pubKey);
        Assert_true(!Bits_memcmp(realIp6, ip6, 16));
    }

    int ifaceIndex = Map_OfSessionsByIp6_put((struct Ip6*)ip6, &sess, &sm->ifaceMap);
    sess->pub.receiveHandle = sm->ifaceMap.handles[ifaceIndex] + sm->firstHandle;

    if (Defined(Log_DEBUG)) {
        uint8_t printedIp6[40];
        AddrTools_printIp(printedIp6, ip6);
        Log_debug(sm->log, "Created session for [%s] handle [%u]",
                  printedIp6, sess->pub.receiveHandle);
    }

    sess->alloc = alloc;
    sess->sessionManager = sm;
    sess->pub.version = version;
    sess->pub.timeOfLastIn = Time_currentTimeMilliseconds(sm->eventBase);
    sess->pub.timeOfKeepAliveIn = Time_currentTimeMilliseconds(sm->eventBase);
    sess->pub.timeOfLastOut = Time_currentTimeMilliseconds(sm->eventBase);
    sess->pub.sendSwitchLabel = label;
    sess->pub.metric = metric;
    sess->pub.maintainSession = maintainSession;
    //Allocator_onFree(alloc, sessionCleanup, sess);
    sendSession(sess, label, 0xffffffff, PFChan_Core_SESSION);
    check(sm, ifaceIndex);
    return sess;
}

static Iface_DEFUN ctrlFrame(struct Message* msg, struct SessionManager_pvt* sm)
{
    struct RouteHeader rh;
    Bits_memset(&rh, 0, RouteHeader_SIZE);
    Er_assert(Message_epop(msg, &rh.sh, SwitchHeader_SIZE));
    Er_assert(Message_epop(msg, NULL, 4));
    rh.flags = RouteHeader_flags_INCOMING | RouteHeader_flags_CTRLMSG;
    Er_assert(Message_epush(msg, &rh, RouteHeader_SIZE));
    return Iface_next(&sm->pub.insideIf, msg);
}

static Iface_DEFUN failedDecrypt(struct Message* msg,
                                 uint64_t label_be,
                                 struct SessionManager_pvt* sm)
{
    Er_assert(Message_epush32be(msg, Error_AUTHENTICATION));
    Er_assert(Message_epush16be(msg, Control_ERROR));
    Er_assert(Message_epush16be(msg, 0));
    uint16_t csum = Checksum_engine(msg->bytes, msg->length);
    Er_assert(Message_epop16be(msg));
    Er_assert(Message_epush16be(msg, csum));

    Er_assert(Message_epush32be(msg, 0xffffffff));

    struct SwitchHeader sh;
    Bits_memset(&sh, 0, SwitchHeader_SIZE);
    SwitchHeader_setSuppressErrors(&sh, true);
    SwitchHeader_setVersion(&sh, SwitchHeader_CURRENT_VERSION);
    sh.label_be = label_be;
    Er_assert(Message_epush(msg, &sh, SwitchHeader_SIZE));

    return Iface_next(&sm->pub.switchIf, msg);
}

static Iface_DEFUN incomingFromSwitchIf(struct Message* msg, struct Iface* iface)
{
    struct SessionManager_pvt* sm =
        Identity_containerOf(iface, struct SessionManager_pvt, pub.switchIf);

    // SwitchHeader, handle, 0 or more bytes of control frame
    if (msg->length < SwitchHeader_SIZE + 4) {
        Log_debug(sm->log, "DROP runt");
        return NULL;
    }

    struct SwitchHeader* switchHeader = (struct SwitchHeader*) msg->bytes;
    Er_assert(Message_eshift(msg, -SwitchHeader_SIZE));

    // The label comes in reversed from the switch because the switch doesn't know that we aren't
    // another switch ready to parse more bits, bit reversing the label yields the source address.
    // (the field is still big endian!)
    switchHeader->label_be = Bits_bitReverse64(switchHeader->label_be);

    struct SessionManager_Session_pvt* session;
    uint32_t nonceOrHandle = Endian_bigEndianToHost32(((uint32_t*)msg->bytes)[0]);
    if (nonceOrHandle == 0xffffffff) {
        Er_assert(Message_eshift(msg, SwitchHeader_SIZE));
        return ctrlFrame(msg, sm);
    }

    // handle, small cryptoAuth header
    if (msg->length < 4 + 20) {
        Log_debug(sm->log, "DROP runt");
        return NULL;
    }

    // This is for handling error situations and being able to send back some kind of a message.
    uint8_t firstSixteen[16];
    uint32_t length0 = msg->length;
    Assert_true(msg->length >= 16);
    Bits_memcpy(firstSixteen, msg->bytes, 16);

    if (nonceOrHandle > 3) {
        // > 3 it's a handle.
        session = sessionForHandle(nonceOrHandle, sm);
        if (!session) {
            Log_debug(sm->log, "DROP message with unrecognized handle [%u]", nonceOrHandle);
            return NULL;
        }
        Er_assert(Message_eshift(msg, -4));
        uint32_t nonce = Endian_bigEndianToHost32(((uint32_t*)msg->bytes)[0]);
        if (nonce < 4) {
            Log_debug(sm->log, "DROP setup message [%u] with specified handle [%u]",
                nonce, nonceOrHandle);
            return NULL;
        }
    } else {
        // handle + big cryptoauth header
        if (msg->length < CryptoHeader_SIZE + 4) {
            Log_debug(sm->log, "DROP runt");
            return NULL;
        }
        struct CryptoHeader* caHeader = (struct CryptoHeader*) msg->bytes;
        uint8_t ip6[16];
        // a packet which claims to be "from us" causes problems
        if (!AddressCalc_addressForPublicKey(ip6, caHeader->publicKey)) {
            Log_debug(sm->log, "DROP Handshake with non-fc key");
            return NULL;
        }

        if (!Bits_memcmp(caHeader->publicKey, sm->cryptoAuth->publicKey, 32)) {
            Log_debug(sm->log, "DROP Handshake from 'ourselves'");
            return NULL;
        }

        uint64_t label = Endian_bigEndianToHost64(switchHeader->label_be);
        session = getSession(sm, ip6, caHeader->publicKey, 0, label, Metric_SM_INCOMING, 0);
        CryptoAuth_resetIfTimeout(session->pub.caSession);
        debugHandlesAndLabel(sm->log, session, label, "new session nonce[%d]", nonceOrHandle);
    }

    bool currentMessageSetup = (nonceOrHandle <= 3);

    enum CryptoAuth_DecryptErr ret = CryptoAuth_decrypt(session->pub.caSession, msg);
    if (ret) {
        debugHandlesAndLabel(sm->log, session,
                             Endian_bigEndianToHost64(switchHeader->label_be),
                             "DROP Failed decrypting message NoH[%d] state[%s]",
                             nonceOrHandle,
                             CryptoAuth_stateString(CryptoAuth_getState(session->pub.caSession)));
        Er_assert(Message_eshift(msg, length0 - msg->length - 24));
        msg->length = 0;
        Er_assert(Message_epush32be(msg, CryptoAuth_getState(session->pub.caSession)));
        Er_assert(Message_epush32be(msg, ret));
        Er_assert(Message_epush(msg, firstSixteen, 16));
        Er_assert(Message_eshift(msg, SwitchHeader_SIZE));
        Assert_true(msg->bytes == (uint8_t*)switchHeader);
        uint64_t label_be = switchHeader->label_be;
        switchHeader->label_be = Bits_bitReverse64(switchHeader->label_be);
        return failedDecrypt(msg, label_be, sm);
    }

    if (currentMessageSetup) {
        session->pub.sendHandle = Er_assert(Message_epop32be(msg));
    }

    Er_assert(Message_eshift(msg, RouteHeader_SIZE));
    struct RouteHeader* header = (struct RouteHeader*) msg->bytes;

    Assert_true(msg->length >= DataHeader_SIZE);
    struct DataHeader* dh = (struct DataHeader*) &header[1];
    if (DataHeader_getContentType(dh) != ContentType_CJDHT) {
        session->pub.timeOfLastIn = Time_currentTimeMilliseconds(sm->eventBase);
    }
    session->pub.bytesIn += msg->length;
    session->pub.timeOfKeepAliveIn = Time_currentTimeMilliseconds(sm->eventBase);

    if (currentMessageSetup) {
        Bits_memcpy(&header->sh, switchHeader, SwitchHeader_SIZE);
        debugHandlesAndLabel0(sm->log,
                              session,
                              Endian_bigEndianToHost64(switchHeader->label_be),
                              "received start message");
    } else {
        // RouteHeader is laid out such that no copy of switch header should be needed.
        Assert_true(&header->sh == switchHeader);
        if (0) { // noisey
        debugHandlesAndLabel0(sm->log,
                              session,
                              Endian_bigEndianToHost64(switchHeader->label_be),
                              "received run message");
        }
    }

    header->version_be = Endian_hostToBigEndian32(session->pub.version);
    Bits_memcpy(header->ip6, session->pub.caSession->herIp6, 16);
    Bits_memcpy(header->publicKey, session->pub.caSession->herPublicKey, 32);

    header->unused = 0;
    header->flags = RouteHeader_flags_INCOMING;

    uint64_t path = Endian_bigEndianToHost64(switchHeader->label_be);
    if (!session->pub.sendSwitchLabel) {
        session->pub.sendSwitchLabel = path;
    }
    if (path != session->pub.recvSwitchLabel) {
        session->pub.recvSwitchLabel = path;
        sendSession(session, path, 0xffffffff, PFChan_Core_DISCOVERED_PATH);
    }

    return Iface_next(&sm->pub.insideIf, msg);
}

static void checkTimedOutBuffers(struct SessionManager_pvt* sm)
{
    for (int i = 0; i < (int)sm->bufMap.count; i++) {
        struct BufferedMessage* buffered = sm->bufMap.values[i];
        int64_t lag = Time_currentTimeMilliseconds(sm->eventBase) - buffered->timeSentMilliseconds;
        if (lag < 10000) { continue; }
        Map_BufferedMessages_remove(i, &sm->bufMap);
        Allocator_free(buffered->alloc);
        i--;
    }
}

static void unsetupSession(struct SessionManager_pvt* sm, struct SessionManager_Session_pvt* sess)
{
    if (!sess->pub.version || !(sess->pub.sendSwitchLabel || sess->pub.recvSwitchLabel)) {
        // Nothing we can do here because it's not ok to send traffic without a version num.
        return;
    }
    struct Allocator* eventAlloc = Allocator_child(sm->alloc);
    struct Message* eventMsg = Message_new(0, 512, eventAlloc);
    struct PFChan_Node n = { .metric_be = Endian_hostToBigEndian32(Metric_DEAD_LINK) };
    n.path_be = Endian_hostToBigEndian64(sess->pub.sendSwitchLabel ?
                                         sess->pub.sendSwitchLabel : sess->pub.recvSwitchLabel);
    n.version_be = Endian_hostToBigEndian32(sess->pub.version);
    n.metric_be = Endian_bigEndianToHost32(sess->pub.metric);
    Bits_memcpy(n.publicKey, sess->pub.caSession->herPublicKey, 32);
    Bits_memcpy(n.ip6, sess->pub.caSession->herIp6, 16);
    Er_assert(Message_epush(eventMsg, &n, PFChan_Node_SIZE));
    Er_assert(Message_epush32be(eventMsg, 0xffffffff));
    Er_assert(Message_epush32be(eventMsg, PFChan_Core_UNSETUP_SESSION));
    Iface_send(&sm->eventIf, eventMsg);
    Allocator_free(eventAlloc);
}

static void triggerSearch(struct SessionManager_pvt* sm, uint8_t target[16], uint32_t version)
{
    struct Allocator* eventAlloc = Allocator_child(sm->alloc);
    struct Message* eventMsg = Message_new(0, 512, eventAlloc);
    Er_assert(Message_epush32be(eventMsg, version));
    Er_assert(Message_epush32be(eventMsg, 0));
    Er_assert(Message_epush(eventMsg, target, 16));
    Er_assert(Message_epush32be(eventMsg, 0xffffffff));
    Er_assert(Message_epush32be(eventMsg, PFChan_Core_SEARCH_REQ));
    Iface_send(&sm->eventIf, eventMsg);
    Allocator_free(eventAlloc);
}

static void checkTimedOutSessions(struct SessionManager_pvt* sm)
{
    for (int i = (int)sm->ifaceMap.count - 1; i >= 0; i--) {
        struct SessionManager_Session_pvt* sess = sm->ifaceMap.values[i];
        int64_t now = Time_currentTimeMilliseconds(sm->eventBase);

        // Check if the session is timed out...
        if (now - sess->pub.timeOfKeepAliveIn > sm->pub.sessionTimeoutMilliseconds) {
            debugSession0(sm->log, sess, "ended");
            sendSession(sess, sess->pub.sendSwitchLabel, 0xffffffff, PFChan_Core_SESSION_ENDED);
            Map_OfSessionsByIp6_remove(i, &sm->ifaceMap);
            Allocator_free(sess->alloc);
            continue;
        }

        if (!sess->pub.maintainSession) {
            // Let pathfinder maintain it's own sessions itself
        } else if (now - sess->pub.lastSearchTime >= sm->pub.sessionSearchAfterMilliseconds) {
            // Session is not in idle state and requires a search
            debugSession0(sm->log, sess, "triggering search");
            triggerSearch(sm, sess->pub.caSession->herIp6, sess->pub.version);
            sess->pub.lastSearchTime = now;
        } else if (CryptoAuth_getState(sess->pub.caSession) < CryptoAuth_State_RECEIVED_KEY) {
            debugSession0(sm->log, sess, "triggering unsetupSession");
            unsetupSession(sm, sess);
        }
    }
}

static void periodically(void* vSessionManager)
{
    struct SessionManager_pvt* sm = Identity_check((struct SessionManager_pvt*) vSessionManager);
    checkTimedOutSessions(sm);
    checkTimedOutBuffers(sm);
}

static void needsLookup(struct SessionManager_pvt* sm, struct Message* msg, bool setupSession)
{
    Assert_true(msg->length >= (RouteHeader_SIZE + DataHeader_SIZE));
    struct RouteHeader* header = (struct RouteHeader*) msg->bytes;

    // We should never be sending CJDHT messages without full version, key, path known.
    struct DataHeader* dataHeader = (struct DataHeader*) &header[1];
    Assert_true(DataHeader_getContentType(dataHeader) != ContentType_CJDHT);

    if (Defined(Log_DEBUG)) {
        uint8_t ipStr[40];
        AddrTools_printIp(ipStr, header->ip6);
        Log_debug(sm->log, "Buffering a packet to [%s] and beginning a search", ipStr);
    }
    int index = Map_BufferedMessages_indexForKey((struct Ip6*)header->ip6, &sm->bufMap);
    if (index > -1) {
        struct BufferedMessage* buffered = sm->bufMap.values[index];
        Map_BufferedMessages_remove(index, &sm->bufMap);
        Allocator_free(buffered->alloc);
        Log_debug(sm->log, "DROP message which needs lookup because new one received");
    }
    if ((int)sm->bufMap.count >= sm->pub.maxBufferedMessages) {
        checkTimedOutBuffers(sm);
        if ((int)sm->bufMap.count >= sm->pub.maxBufferedMessages) {
            Log_debug(sm->log, "DROP message needing lookup maxBufferedMessages ([%d]) is reached",
                      sm->pub.maxBufferedMessages);
            return;
        }
    }
    struct Allocator* lookupAlloc = Allocator_child(sm->alloc);
    struct BufferedMessage* buffered =
        Allocator_calloc(lookupAlloc, sizeof(struct BufferedMessage), 1);
    buffered->msg = msg;
    buffered->alloc = lookupAlloc;
    buffered->timeSentMilliseconds = Time_currentTimeMilliseconds(sm->eventBase);
    Allocator_adopt(lookupAlloc, msg->alloc);
    Assert_true(Map_BufferedMessages_put((struct Ip6*)header->ip6, &buffered, &sm->bufMap) > -1);

    triggerSearch(sm, header->ip6, Endian_hostToBigEndian32(header->version_be));
}

static Iface_DEFUN readyToSend(struct Message* msg,
                               struct SessionManager_pvt* sm,
                               struct SessionManager_Session_pvt* sess)
{
    struct RouteHeader* header = (struct RouteHeader*) msg->bytes;
    struct DataHeader* dh = (struct DataHeader*) &header[1];
    if (DataHeader_getContentType(dh) != ContentType_CJDHT) {
        sess->pub.timeOfLastOut = Time_currentTimeMilliseconds(sm->eventBase);
    }
    Er_assert(Message_eshift(msg, -RouteHeader_SIZE));
    struct SwitchHeader* sh;
    CryptoAuth_resetIfTimeout(sess->pub.caSession);
    if (CryptoAuth_getState(sess->pub.caSession) < CryptoAuth_State_RECEIVED_KEY) {
        // Put the handle into the message so that it's authenticated.
        Er_assert(Message_epush32be(msg, sess->pub.receiveHandle));

        // Copy back the SwitchHeader so it is not clobbered.
        Er_assert(Message_eshift(msg, (CryptoHeader_SIZE + SwitchHeader_SIZE)));
        Bits_memcpy(msg->bytes, &header->sh, SwitchHeader_SIZE);
        sh = (struct SwitchHeader*) msg->bytes;
        Er_assert(Message_eshift(msg, -(CryptoHeader_SIZE + SwitchHeader_SIZE)));
    } else {
        sh = &header->sh;
    }

    // This pointer ceases to be useful.
    header = NULL;

    sess->pub.bytesOut += msg->length;

    Assert_true(!CryptoAuth_encrypt(sess->pub.caSession, msg));

    if (CryptoAuth_getState(sess->pub.caSession) >= CryptoAuth_State_RECEIVED_KEY) {
        if (0) { // Noisy
            debugHandlesAndLabel0(sm->log,
                                  sess,
                                  Endian_bigEndianToHost64(sh->label_be),
                                  "sending run message");
        }
        Er_assert(Message_epush32be(msg, sess->pub.sendHandle));
    } else {
        debugHandlesAndLabel0(sm->log,
                              sess,
                              Endian_bigEndianToHost64(sh->label_be),
                              "sending start message");
    }

    // The SwitchHeader should have been moved to the correct location.
    Er_assert(Message_eshift(msg, SwitchHeader_SIZE));
    Assert_true((uint8_t*)sh == msg->bytes);

    if (!sh->label_be) {
        Bits_memset(sh, 0, SwitchHeader_SIZE);
        sh->label_be = Endian_hostToBigEndian64(sess->pub.sendSwitchLabel);
        SwitchHeader_setVersion(sh, SwitchHeader_CURRENT_VERSION);
    }

    return Iface_next(&sm->pub.switchIf, msg);
}

static Iface_DEFUN outgoingCtrlFrame(struct Message* msg, struct SessionManager_pvt* sm)
{
    Assert_true(msg->length >= RouteHeader_SIZE);
    struct RouteHeader* header = (struct RouteHeader*) msg->bytes;
    if (!Bits_isZero(header->publicKey, 32) || !Bits_isZero(header->ip6, 16)) {
        Log_debug(sm->log, "DROP Ctrl frame with non-zero destination key or IP");
        return NULL;
    }
    if (!(header->flags & RouteHeader_flags_CTRLMSG)) {
        Log_debug(sm->log, "DROP Ctrl frame w/o RouteHeader_flags_CTRLMSG flag");
        return NULL;
    }
    struct SwitchHeader sh;
    Bits_memcpy(&sh, &header->sh, SwitchHeader_SIZE);
    Er_assert(Message_epop(msg, NULL, RouteHeader_SIZE));
    Er_assert(Message_epush32be(msg, 0xffffffff));
    Er_assert(Message_epush(msg, &sh, SwitchHeader_SIZE));
    return Iface_next(&sm->pub.switchIf, msg);
}

static Iface_DEFUN incomingFromInsideIf(struct Message* msg, struct Iface* iface)
{
    struct SessionManager_pvt* sm =
        Identity_containerOf(iface, struct SessionManager_pvt, pub.insideIf);
    Assert_true(msg->length >= RouteHeader_SIZE);
    struct RouteHeader* header = (struct RouteHeader*) msg->bytes;
    if (header->flags & RouteHeader_flags_CTRLMSG) {
        return outgoingCtrlFrame(msg, sm);
    }
    Assert_true(msg->length >= RouteHeader_SIZE + DataHeader_SIZE);
    struct DataHeader* dataHeader = (struct DataHeader*) &header[1];

    struct SessionManager_Session_pvt* sess = sessionForIp6(header->ip6, sm);
    if (!sess) {
        if (!Bits_isZero(header->publicKey, 32) && header->version_be) {
            sess = getSession(sm,
                              header->ip6,
                              header->publicKey,
                              Endian_bigEndianToHost32(header->version_be),
                              Endian_bigEndianToHost64(header->sh.label_be),
                              Metric_SM_SEND,
                              !(header->flags & RouteHeader_flags_PATHFINDER));
        } else {
            needsLookup(sm, msg, false);
            return NULL;
        }
    }

    if (header->version_be) { sess->pub.version = Endian_bigEndianToHost32(header->version_be); }

    if (!sess->pub.version) {
        needsLookup(sm, msg, false);
        return NULL;
    }

    if (header->sh.label_be) {
        // fallthrough
    } else if (sess->pub.sendSwitchLabel) {
        Bits_memset(&header->sh, 0, SwitchHeader_SIZE);
        header->sh.label_be = Endian_hostToBigEndian64(sess->pub.sendSwitchLabel);
        SwitchHeader_setVersion(&header->sh, SwitchHeader_CURRENT_VERSION);
    } else {
        needsLookup(sm, msg, false);
        return NULL;
    }

    // Forward secrecy, only send dht messages until the session is setup.
    CryptoAuth_resetIfTimeout(sess->pub.caSession);
    if (DataHeader_getContentType(dataHeader) != ContentType_CJDHT &&
        CryptoAuth_getState(sess->pub.caSession) < CryptoAuth_State_RECEIVED_KEY)
    {
        needsLookup(sm, msg, true);
        return NULL;
    }

    return readyToSend(msg, sm, sess);
}

static Iface_DEFUN sessions(struct SessionManager_pvt* sm,
                            uint32_t sourcePf,
                            struct Allocator* tempAlloc)
{
    for (int i = 0; i < (int)sm->ifaceMap.count; i++) {
        struct SessionManager_Session_pvt* sess = sm->ifaceMap.values[i];
        sendSession(sess, sess->pub.sendSwitchLabel, sourcePf, PFChan_Core_SESSION);
    }
    return NULL;
}

static Iface_DEFUN incomingFromEventIf(struct Message* msg, struct Iface* iface)
{
    struct SessionManager_pvt* sm = Identity_containerOf(iface, struct SessionManager_pvt, eventIf);
    enum PFChan_Pathfinder ev = Er_assert(Message_epop32be(msg));
    uint32_t sourcePf = Er_assert(Message_epop32be(msg));
    if (ev == PFChan_Pathfinder_SESSIONS) {
        Assert_true(!msg->length);
        return sessions(sm, sourcePf, msg->alloc);
    }
    Assert_true(ev == PFChan_Pathfinder_NODE);

    struct PFChan_Node node;
    Er_assert(Message_epop(msg, &node, PFChan_Node_SIZE));
    Assert_true(!msg->length);
    int index = Map_BufferedMessages_indexForKey((struct Ip6*)node.ip6, &sm->bufMap);
    struct SessionManager_Session_pvt* sess = sessionForIp6(node.ip6, sm);
    if (!sess) {
        // Node we don't care about.
        if (index == -1) { return NULL; }
        // Broken path to a node we don't have a session for...
        if (node.metric_be == Metric_DEAD_LINK) { return NULL; }
    }
    sess = getSession(sm,
                      node.ip6,
                      node.publicKey,
                      Endian_bigEndianToHost32(node.version_be),
                      Endian_bigEndianToHost64(node.path_be),
                      Endian_bigEndianToHost32(node.metric_be),
                      0);

    // Send what's on the buffer...
    if (index > -1 && CryptoAuth_getState(sess->pub.caSession) >= CryptoAuth_State_RECEIVED_KEY) {
        struct BufferedMessage* bm = sm->bufMap.values[index];
        Iface_CALL(readyToSend, bm->msg, sm, sess);
        Map_BufferedMessages_remove(index, &sm->bufMap);
        Allocator_free(bm->alloc);
    } else if (CryptoAuth_getState(sess->pub.caSession) < CryptoAuth_State_RECEIVED_KEY) {
        unsetupSession(sm, sess);
    }
    return NULL;
}

struct SessionManager* SessionManager_new(struct Allocator* allocator,
                                          struct EventBase* eventBase,
                                          struct CryptoAuth* cryptoAuth,
                                          struct Random* rand,
                                          struct Log* log,
                                          struct EventEmitter* ee)
{
    struct Allocator* alloc = Allocator_child(allocator);
    struct SessionManager_pvt* sm = Allocator_calloc(alloc, sizeof(struct SessionManager_pvt), 1);
    sm->alloc = alloc;
    sm->pub.switchIf.send = incomingFromSwitchIf;
    sm->pub.insideIf.send = incomingFromInsideIf;
    sm->bufMap.allocator = alloc;
    sm->ifaceMap.allocator = alloc;
    sm->log = log;
    sm->cryptoAuth = cryptoAuth;
    sm->eventBase = eventBase;
    sm->pub.sessionTimeoutMilliseconds = SessionManager_SESSION_TIMEOUT_MILLISECONDS_DEFAULT;
    sm->pub.maxBufferedMessages = SessionManager_MAX_BUFFERED_MESSAGES_DEFAULT;
    sm->pub.sessionSearchAfterMilliseconds =
        SessionManager_SESSION_SEARCH_AFTER_MILLISECONDS_DEFAULT;

    sm->eventIf.send = incomingFromEventIf;
    EventEmitter_regCore(ee, &sm->eventIf, PFChan_Pathfinder_NODE);
    EventEmitter_regCore(ee, &sm->eventIf, PFChan_Pathfinder_SESSIONS);

    sm->firstHandle =
        (Random_uint32(rand) % (MAX_FIRST_HANDLE - MIN_FIRST_HANDLE)) + MIN_FIRST_HANDLE;

    Timeout_setInterval(periodically, sm, 10000, eventBase, alloc);

    Identity_set(sm);

    return &sm->pub;
}
