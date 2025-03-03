// Protocol Buffers - Google's data interchange format
// Copyright 2008 Google Inc.  All rights reserved.
// https://developers.google.com/protocol-buffers/
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
// copyright notice, this list of conditions and the following disclaimer
// in the documentation and/or other materials provided with the
// distribution.
//     * Neither the name of Google Inc. nor the names of its
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

// Author: kenton@google.com (Kenton Varda)
//  Based on original Protocol Buffers design by
//  Sanjay Ghemawat, Jeff Dean, and others.

#ifndef GOOGLE_PROTOBUF_COMPILER_JAVA_MESSAGE_FIELD_LITE_H__
#define GOOGLE_PROTOBUF_COMPILER_JAVA_MESSAGE_FIELD_LITE_H__

#include <cstdint>
#include <map>
#include <string>

#include "google/protobuf/compiler/java/field.h"

namespace google {
namespace protobuf {
namespace compiler {
namespace java {
class Context;            // context.h
class ClassNameResolver;  // name_resolver.h
}  // namespace java
}  // namespace compiler
}  // namespace protobuf
}  // namespace google

namespace google {
namespace protobuf {
namespace compiler {
namespace java {

class ImmutableMessageFieldLiteGenerator : public ImmutableFieldLiteGenerator {
 public:
  explicit ImmutableMessageFieldLiteGenerator(const FieldDescriptor* descriptor,
                                              int messageBitIndex,
                                              Context* context);
  ImmutableMessageFieldLiteGenerator(
      const ImmutableMessageFieldLiteGenerator&) = delete;
  ImmutableMessageFieldLiteGenerator& operator=(
      const ImmutableMessageFieldLiteGenerator&) = delete;
  ~ImmutableMessageFieldLiteGenerator() override;

  // implements ImmutableFieldLiteGenerator
  // ------------------------------------
  int GetNumBitsForMessage() const override;
  void GenerateInterfaceMembers(io::Printer* printer) const override;
  void GenerateMembers(io::Printer* printer) const override;
  void GenerateBuilderMembers(io::Printer* printer) const override;
  void GenerateInitializationCode(io::Printer* printer) const override;
  void GenerateFieldInfo(io::Printer* printer,
                         std::vector<uint16_t>* output) const override;
  void GenerateKotlinDslMembers(io::Printer* printer) const override;

  std::string GetBoxedType() const override;

 protected:
  const FieldDescriptor* descriptor_;
  absl::flat_hash_map<absl::string_view, std::string> variables_;
  const int messageBitIndex_;
  ClassNameResolver* name_resolver_;
  Context* context_;

 private:
  void GenerateKotlinOrNull(io::Printer* printer) const;
};

class ImmutableMessageOneofFieldLiteGenerator
    : public ImmutableMessageFieldLiteGenerator {
 public:
  ImmutableMessageOneofFieldLiteGenerator(const FieldDescriptor* descriptor,
                                          int messageBitIndex,
                                          Context* context);
  ImmutableMessageOneofFieldLiteGenerator(
      const ImmutableMessageOneofFieldLiteGenerator&) = delete;
  ImmutableMessageOneofFieldLiteGenerator& operator=(
      const ImmutableMessageOneofFieldLiteGenerator&) = delete;
  ~ImmutableMessageOneofFieldLiteGenerator() override;

  void GenerateMembers(io::Printer* printer) const override;
  void GenerateBuilderMembers(io::Printer* printer) const override;
  void GenerateFieldInfo(io::Printer* printer,
                         std::vector<uint16_t>* output) const override;

};

class RepeatedImmutableMessageFieldLiteGenerator
    : public ImmutableFieldLiteGenerator {
 public:
  explicit RepeatedImmutableMessageFieldLiteGenerator(
      const FieldDescriptor* descriptor, int messageBitIndex, Context* context);
  RepeatedImmutableMessageFieldLiteGenerator(
      const RepeatedImmutableMessageFieldLiteGenerator&) = delete;
  RepeatedImmutableMessageFieldLiteGenerator& operator=(
      const RepeatedImmutableMessageFieldLiteGenerator&) = delete;
  ~RepeatedImmutableMessageFieldLiteGenerator() override;

  // implements ImmutableFieldLiteGenerator ------------------------------------
  int GetNumBitsForMessage() const override;
  void GenerateInterfaceMembers(io::Printer* printer) const override;
  void GenerateMembers(io::Printer* printer) const override;
  void GenerateBuilderMembers(io::Printer* printer) const override;
  void GenerateInitializationCode(io::Printer* printer) const override;
  void GenerateFieldInfo(io::Printer* printer,
                         std::vector<uint16_t>* output) const override;
  void GenerateKotlinDslMembers(io::Printer* printer) const override;

  std::string GetBoxedType() const override;

 protected:
  const FieldDescriptor* descriptor_;
  absl::flat_hash_map<absl::string_view, std::string> variables_;
  ClassNameResolver* name_resolver_;
  Context* context_;
};

}  // namespace java
}  // namespace compiler
}  // namespace protobuf
}  // namespace google

#endif  // GOOGLE_PROTOBUF_COMPILER_JAVA_MESSAGE_FIELD_LITE_H__
