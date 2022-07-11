/** Copyright 2020-2021 Alibaba Group Holding Limited.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

#ifndef SRC_COMMON_UTIL_UUID_H_
#define SRC_COMMON_UTIL_UUID_H_

// The __VPP macro is used to avoid including <x86intrin.h> when parsing
// and coding with libclang/clang tooling.
#include <cstdint>
#if defined(__x86_64__) && !defined(__VPP)
#include <x86intrin.h>
#endif

#include <cstdlib>
#include <limits>
#include <string>

#include "common/util/base64.h"

namespace vineyard {

/**
 * @brief ObjectID is an opaque type for vineyard's object id. The object ID is
 * generated by vineyard server, the underlying type of ObjectID is a 64-bit
 * unsigned integer.
 */
using ObjectID = uint64_t;

/**
 * @brief Signature is an opaque type for vineyard's object. The signature of
 * an object keep unchange during migration. The underlying type of Signature
 * is a 64-bit unsigned integer.
 */
using Signature = uint64_t;

/**
 * @brief InstanceID is an opaque type for vineyard's instance. The
 * underlying type of Instance is a 64-bit unsigned integer.
 */
using InstanceID = uint64_t;

/**
 * @brief SessionID is an opaque type for vineyard's Session. The
 * underlying type of SessionID is a 64-bit unsigned integer.
 */
using SessionID = int64_t;

/**
 * @brief PlasmaID is an opaque type for vineyard's PlasmaPayload. The
 * underlying type of PlasmaID is base64 string for compatibility.
 */
using PlasmaID = std::string;

template <typename T, typename F>
auto static_if(std::true_type, T t, F f) {
  return t;
}

template <typename T, typename F>
auto static_if(std::false_type, T t, F f) {
  return f;
}

template <bool B, typename T, typename F>
auto static_if(T t, F f) {
  return static_if(std::integral_constant<bool, B>{}, t, f);
}

template <bool B, typename T>
auto static_if(T t) {
  return static_if(std::integral_constant<bool, B>{}, t, [](auto&&...) {});
}

/*
 *  @brief Make empty blob and preallocate blob always mapping to the same place
 *         Others will be mapped randomly between
 * (0x8000000000000000UL,0xFFFFFFFFFFFFFFFFUL) exclusively.
 */
inline ObjectID GenerateBlobID(const uintptr_t ptr) {
  if (ptr == 0x8000000000000000UL ||
      ptr == std::numeric_limits<uintptr_t>::max()) {
    return static_cast<uint64_t>(ptr) | 0x8000000000000000UL;
  }
#if defined(__x86_64__)
  auto rd = __rdtsc() % (0x7FFFFFFFFFFFFFFFUL - 2) + 1;
  return (0x7FFFFFFFFFFFFFFFUL & static_cast<uint64_t>(rd)) |
         0x8000000000000000UL;
#else
  auto rd =
      rand() % (0x7FFFFFFFFFFFFFFFUL - 2) + 1;  // NOLINT(runtime/threadsafe_fn)
  return 0x8000000000000000UL |
         (0x7FFFFFFFFFFFFFFFUL & static_cast<uint64_t>(rd));
#endif
}

inline SessionID GenerateSessionID() {
#if defined(__x86_64__)
  return 0x7FFFFFFFFFFFFFFFUL & static_cast<uint64_t>(__rdtsc());
#else
  return 0x7FFFFFFFFFFFFFFFUL &
         static_cast<uint64_t>(rand());  // NOLINT(runtime/threadsafe_fn)
#endif
}

inline ObjectID GenerateObjectID() {
#if defined(__x86_64__) && !defined(__VPP)
  return 0x7FFFFFFFFFFFFFFFUL & static_cast<uint64_t>(__rdtsc());
#else
  return 0x7FFFFFFFFFFFFFFFUL &
         static_cast<uint64_t>(rand());  // NOLINT(runtime/threadsafe_fn)
#endif
}

inline ObjectID GenerateSignature() {
#if defined(__x86_64__)
  return 0x7FFFFFFFFFFFFFFFUL & static_cast<uint64_t>(__rdtsc());
#else
  return 0x7FFFFFFFFFFFFFFFUL &
         static_cast<uint64_t>(rand());  // NOLINT(runtime/threadsafe_fn)
#endif
}

inline bool IsBlob(ObjectID id) { return id & 0x8000000000000000UL; }

const std::string ObjectIDToString(const ObjectID id);

inline std::string const PlasmaIDToString(PlasmaID const plasma_id) {
  return base64_decode(std::string(plasma_id));
}

inline ObjectID ObjectIDFromString(const std::string& s) {
  return strtoull(s.c_str() + 1, nullptr, 16);
}

inline ObjectID ObjectIDFromString(const char* s) {
  return strtoull(s + 1, nullptr, 16);
}

// TODO base64 encoding
inline PlasmaID PlasmaIDFromString(std::string const& s) {
  return PlasmaID(base64_encode(s));
}

inline PlasmaID PlasmaIDFromString(const char* s) {
  return PlasmaID(base64_encode(s));
}

constexpr inline SessionID RootSessionID() { return 0x0000000000000000UL; }

const std::string SessionIDToString(const SessionID id);

inline SessionID SessionIDFromString(const std::string& s) {
  return strtoull(s.c_str() + 1, nullptr, 16);
}

inline SessionID SessionIDFromString(const char* s) {
  return strtoull(s + 1, nullptr, 16);
}

[[deprecated(
    "For backwards-compatiblity, will be removed in 1.0.")]] inline const std::
    string
    VYObjectIDToString(const ObjectID id) {
  return ObjectIDToString(id);
}

[[deprecated(
    "For backwards-compatiblity, will be removed in 1.0.")]] inline ObjectID
VYObjectIDFromString(const std::string& s) {
  return ObjectIDFromString(s);
}

[[deprecated(
    "For backwards-compatiblity, will be removed in 1.0.")]] inline ObjectID
VYObjectIDFromString(const char* s) {
  return ObjectIDFromString(s);
}

const std::string SignatureToString(const Signature id);

inline Signature SignatureFromString(const std::string& s) {
  return strtoull(s.c_str() + 1, nullptr, 16);
}

inline Signature SignatureFromString(const char* s) {
  return strtoull(s + 1, nullptr, 16);
}

inline ObjectID InvalidObjectID() {
  return std::numeric_limits<ObjectID>::max();
}

inline ObjectID InvalidSignature() {
  return std::numeric_limits<Signature>::max();
}

inline InstanceID UnspecifiedInstanceID() {
  return std::numeric_limits<InstanceID>::max();
}

template <typename ID>
inline ID GenerateBlobID(uintptr_t ptr) {
  uint64_t ans = GenerateBlobID(ptr);
  return static_if<std::is_same<ID, ObjectID>{}>(
      [&]() { return ObjectID(ans); },
      [&]() { return PlasmaIDFromString(ObjectIDToString(ObjectID(ans))); })();
}

template <typename ID>
inline ID GenerateBlobID(const void* ptr) {
  uint64_t ans = GenerateBlobID(reinterpret_cast<const uintptr_t>(ptr));
  return static_if<std::is_same<ID, ObjectID>{}>(
      [&]() { return ObjectID(ans); },
      [&]() { return PlasmaIDFromString(ObjectIDToString(ObjectID(ans))); })();
}

template <typename ID = ObjectID>
ID EmptyBlobID() {
  return GenerateBlobID<ID>(0x8000000000000000UL);
}

template <typename ID>
std::string IDToString(ID id) {
  return static_if<std::is_same<ID, ObjectID>{}>(
      [](ObjectID& id) { return ObjectIDToString(id); },
      [](PlasmaID& id) { return PlasmaIDToString(id); })(id);
}

}  // namespace vineyard

#endif  // SRC_COMMON_UTIL_UUID_H_
