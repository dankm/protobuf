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

// Author: dweis@google.com (Daniel Weis)
//  Based on original Protocol Buffers design by
//  Sanjay Ghemawat, Jeff Dean, and others.

#include "google/protobuf/compiler/java/message_builder.h"

#include <algorithm>
#include <memory>
#include <vector>

#include "google/protobuf/io/coded_stream.h"
#include "google/protobuf/io/printer.h"
#include "google/protobuf/wire_format.h"
#include "absl/container/flat_hash_map.h"
#include "absl/strings/ascii.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/substitute.h"
#include "google/protobuf/compiler/java/context.h"
#include "google/protobuf/compiler/java/doc_comment.h"
#include "google/protobuf/compiler/java/enum.h"
#include "google/protobuf/compiler/java/extension.h"
#include "google/protobuf/compiler/java/generator_factory.h"
#include "google/protobuf/compiler/java/helpers.h"
#include "google/protobuf/compiler/java/name_resolver.h"
#include "google/protobuf/descriptor.pb.h"

// Must be last.
#include "google/protobuf/port_def.inc"

namespace google {
namespace protobuf {
namespace compiler {
namespace java {

using internal::WireFormat;
using internal::WireFormatLite;

namespace {
std::string MapValueImmutableClassdName(const Descriptor* descriptor,
                                        ClassNameResolver* name_resolver) {
  const FieldDescriptor* value_field = descriptor->map_value();
  GOOGLE_CHECK_EQ(FieldDescriptor::TYPE_MESSAGE, value_field->type());
  return name_resolver->GetImmutableClassName(value_field->message_type());
}
}  // namespace

MessageBuilderGenerator::MessageBuilderGenerator(const Descriptor* descriptor,
                                                 Context* context)
    : descriptor_(descriptor),
      context_(context),
      name_resolver_(context->GetNameResolver()),
      field_generators_(descriptor, context_) {
  GOOGLE_CHECK(HasDescriptorMethods(descriptor->file(), context->EnforceLite()))
      << "Generator factory error: A non-lite message generator is used to "
         "generate lite messages.";
  for (int i = 0; i < descriptor_->field_count(); i++) {
    if (IsRealOneof(descriptor_->field(i))) {
      oneofs_.insert(descriptor_->field(i)->containing_oneof());
    }
  }
}

MessageBuilderGenerator::~MessageBuilderGenerator() {}

void MessageBuilderGenerator::Generate(io::Printer* printer) {
  WriteMessageDocComment(printer, descriptor_);
  if (descriptor_->extension_range_count() > 0) {
    printer->Print(
        "public static final class Builder extends\n"
        "    com.google.protobuf.GeneratedMessage$ver$.ExtendableBuilder<\n"
        "      $classname$, Builder> implements\n"
        "    $extra_interfaces$\n"
        "    $classname$OrBuilder {\n",
        "classname", name_resolver_->GetImmutableClassName(descriptor_),
        "extra_interfaces", ExtraBuilderInterfaces(descriptor_), "ver",
        GeneratedCodeVersionSuffix());
  } else {
    printer->Print(
        "public static final class Builder extends\n"
        "    com.google.protobuf.GeneratedMessage$ver$.Builder<Builder> "
        "implements\n"
        "    $extra_interfaces$\n"
        "    $classname$OrBuilder {\n",
        "classname", name_resolver_->GetImmutableClassName(descriptor_),
        "extra_interfaces", ExtraBuilderInterfaces(descriptor_), "ver",
        GeneratedCodeVersionSuffix());
  }
  printer->Indent();

  GenerateDescriptorMethods(printer);
  GenerateCommonBuilderMethods(printer);

  if (context_->HasGeneratedMethods(descriptor_)) {
    GenerateIsInitialized(printer);
    GenerateBuilderParsingMethods(printer);
  }

  // oneof
  absl::flat_hash_map<absl::string_view, std::string> vars;
  for (auto oneof : oneofs_) {
    vars["oneof_name"] = context_->GetOneofGeneratorInfo(oneof)->name;
    vars["oneof_capitalized_name"] =
        context_->GetOneofGeneratorInfo(oneof)->capitalized_name;
    vars["oneof_index"] = absl::StrCat(oneof->index());
    // oneofCase_ and oneof_
    printer->Print(vars,
                   "private int $oneof_name$Case_ = 0;\n"
                   "private java.lang.Object $oneof_name$_;\n");
    // oneofCase() and clearOneof()
    printer->Print(vars,
                   "public $oneof_capitalized_name$Case\n"
                   "    get$oneof_capitalized_name$Case() {\n"
                   "  return $oneof_capitalized_name$Case.forNumber(\n"
                   "      $oneof_name$Case_);\n"
                   "}\n"
                   "\n"
                   "public Builder clear$oneof_capitalized_name$() {\n"
                   "  $oneof_name$Case_ = 0;\n"
                   "  $oneof_name$_ = null;\n");
    printer->Print("  onChanged();\n");
    printer->Print(
        "  return this;\n"
        "}\n"
        "\n");
  }

  // Integers for bit fields.
  int totalBits = 0;
  for (int i = 0; i < descriptor_->field_count(); i++) {
    totalBits +=
        field_generators_.get(descriptor_->field(i)).GetNumBitsForBuilder();
  }
  int totalInts = (totalBits + 31) / 32;
  for (int i = 0; i < totalInts; i++) {
    printer->Print("private int $bit_field_name$;\n", "bit_field_name",
                   GetBitFieldName(i));
  }

  for (int i = 0; i < descriptor_->field_count(); i++) {
    printer->Print("\n");
    field_generators_.get(descriptor_->field(i))
        .GenerateBuilderMembers(printer);
  }

  if (context_->options().opensource_runtime) {
    // Override methods declared in GeneratedMessage to return the concrete
    // generated type so callsites won't depend on GeneratedMessage. This
    // is needed to keep binary compatibility when we change generated code
    // to subclass a different GeneratedMessage class (e.g., in v3.0.0 release
    // we changed all generated code to subclass GeneratedMessageV3).
    printer->Print(
        "@java.lang.Override\n"
        "public final Builder setUnknownFields(\n"
        "    final com.google.protobuf.UnknownFieldSet unknownFields) {\n"
        "  return super.setUnknownFields(unknownFields);\n"
        "}\n"
        "\n"
        "@java.lang.Override\n"
        "public final Builder mergeUnknownFields(\n"
        "    final com.google.protobuf.UnknownFieldSet unknownFields) {\n"
        "  return super.mergeUnknownFields(unknownFields);\n"
        "}\n"
        "\n");
  }

  printer->Print(
      "\n"
      "// @@protoc_insertion_point(builder_scope:$full_name$)\n",
      "full_name", descriptor_->full_name());

  printer->Outdent();
  printer->Print("}\n");
}

// ===================================================================

void MessageBuilderGenerator::GenerateDescriptorMethods(io::Printer* printer) {
  if (!descriptor_->options().no_standard_descriptor_accessor()) {
    printer->Print(
        "public static final com.google.protobuf.Descriptors.Descriptor\n"
        "    getDescriptor() {\n"
        "  return $fileclass$.internal_$identifier$_descriptor;\n"
        "}\n"
        "\n",
        "fileclass", name_resolver_->GetImmutableClassName(descriptor_->file()),
        "identifier", UniqueFileScopeIdentifier(descriptor_));
  }
  std::vector<const FieldDescriptor*> map_fields;
  for (int i = 0; i < descriptor_->field_count(); i++) {
    const FieldDescriptor* field = descriptor_->field(i);
    if (GetJavaType(field) == JAVATYPE_MESSAGE &&
        IsMapEntry(field->message_type())) {
      map_fields.push_back(field);
    }
  }
  if (!map_fields.empty()) {
    printer->Print(
        "@SuppressWarnings({\"rawtypes\"})\n"
        "protected com.google.protobuf.MapField internalGetMapField(\n"
        "    int number) {\n"
        "  switch (number) {\n");
    printer->Indent();
    printer->Indent();
    for (int i = 0; i < map_fields.size(); ++i) {
      const FieldDescriptor* field = map_fields[i];
      const FieldGeneratorInfo* info = context_->GetFieldGeneratorInfo(field);
      printer->Print(
          "case $number$:\n"
          "  return internalGet$capitalized_name$();\n",
          "number", absl::StrCat(field->number()), "capitalized_name",
          info->capitalized_name);
    }
    printer->Print(
        "default:\n"
        "  throw new RuntimeException(\n"
        "      \"Invalid map field number: \" + number);\n");
    printer->Outdent();
    printer->Outdent();
    printer->Print(
        "  }\n"
        "}\n");
    printer->Print(
        "@SuppressWarnings({\"rawtypes\"})\n"
        "protected com.google.protobuf.MapField internalGetMutableMapField(\n"
        "    int number) {\n"
        "  switch (number) {\n");
    printer->Indent();
    printer->Indent();
    for (int i = 0; i < map_fields.size(); ++i) {
      const FieldDescriptor* field = map_fields[i];
      const FieldGeneratorInfo* info = context_->GetFieldGeneratorInfo(field);
      printer->Print(
          "case $number$:\n"
          "  return internalGetMutable$capitalized_name$();\n",
          "number", absl::StrCat(field->number()), "capitalized_name",
          info->capitalized_name);
    }
    printer->Print(
        "default:\n"
        "  throw new RuntimeException(\n"
        "      \"Invalid map field number: \" + number);\n");
    printer->Outdent();
    printer->Outdent();
    printer->Print(
        "  }\n"
        "}\n");
  }
  printer->Print(
      "@java.lang.Override\n"
      "protected com.google.protobuf.GeneratedMessage$ver$.FieldAccessorTable\n"
      "    internalGetFieldAccessorTable() {\n"
      "  return $fileclass$.internal_$identifier$_fieldAccessorTable\n"
      "      .ensureFieldAccessorsInitialized(\n"
      "          $classname$.class, $classname$.Builder.class);\n"
      "}\n"
      "\n",
      "classname", name_resolver_->GetImmutableClassName(descriptor_),
      "fileclass", name_resolver_->GetImmutableClassName(descriptor_->file()),
      "identifier", UniqueFileScopeIdentifier(descriptor_), "ver",
      GeneratedCodeVersionSuffix());
}

// ===================================================================

void MessageBuilderGenerator::GenerateCommonBuilderMethods(
    io::Printer* printer) {
  // Decide if we really need to have the "maybeForceBuilderInitialization()"
  // method.
  // TODO(b/249158148): Remove the need for this entirely
  bool need_maybe_force_builder_init = false;
  for (int i = 0; i < descriptor_->field_count(); i++) {
    if (descriptor_->field(i)->message_type() != nullptr &&
        !IsRealOneof(descriptor_->field(i)) &&
        HasHasbit(descriptor_->field(i))) {
      need_maybe_force_builder_init = true;
      break;
    }
  }

  const char* force_builder_init = need_maybe_force_builder_init
                                       ? "  maybeForceBuilderInitialization();"
                                       : "";

  printer->Print(
      "// Construct using $classname$.newBuilder()\n"
      "private Builder() {\n"
      "$force_builder_init$\n"
      "}\n"
      "\n",
      "classname", name_resolver_->GetImmutableClassName(descriptor_),
      "force_builder_init", force_builder_init);

  printer->Print(
      "private Builder(\n"
      "    com.google.protobuf.GeneratedMessage$ver$.BuilderParent parent) {\n"
      "  super(parent);\n"
      "$force_builder_init$\n"
      "}\n",
      "classname", name_resolver_->GetImmutableClassName(descriptor_), "ver",
      GeneratedCodeVersionSuffix(), "force_builder_init", force_builder_init);

  if (need_maybe_force_builder_init) {
    printer->Print(
        "private void maybeForceBuilderInitialization() {\n"
        "  if (com.google.protobuf.GeneratedMessage$ver$\n"
        "          .alwaysUseFieldBuilders) {\n",
        "ver", GeneratedCodeVersionSuffix());

    printer->Indent();
    printer->Indent();
    for (int i = 0; i < descriptor_->field_count(); i++) {
      if (!IsRealOneof(descriptor_->field(i))) {
        field_generators_.get(descriptor_->field(i))
            .GenerateFieldBuilderInitializationCode(printer);
      }
    }
    printer->Outdent();
    printer->Outdent();

    printer->Print(
        "  }\n"
        "}\n");
  }

  printer->Print(
      "@java.lang.Override\n"
      "public Builder clear() {\n"
      "  super.clear();\n");

  printer->Indent();

  for (int i = 0; i < descriptor_->field_count(); i++) {
    field_generators_.get(descriptor_->field(i))
        .GenerateBuilderClearCode(printer);
  }

  for (auto oneof : oneofs_) {
    printer->Print(
        "$oneof_name$Case_ = 0;\n"
        "$oneof_name$_ = null;\n",
        "oneof_name", context_->GetOneofGeneratorInfo(oneof)->name);
  }

  printer->Outdent();

  printer->Print(
      "  return this;\n"
      "}\n"
      "\n");

  printer->Print(
      "@java.lang.Override\n"
      "public com.google.protobuf.Descriptors.Descriptor\n"
      "    getDescriptorForType() {\n"
      "  return $fileclass$.internal_$identifier$_descriptor;\n"
      "}\n"
      "\n",
      "fileclass", name_resolver_->GetImmutableClassName(descriptor_->file()),
      "identifier", UniqueFileScopeIdentifier(descriptor_));

  // LITE runtime implements this in GeneratedMessageLite.
  printer->Print(
      "@java.lang.Override\n"
      "public $classname$ getDefaultInstanceForType() {\n"
      "  return $classname$.getDefaultInstance();\n"
      "}\n"
      "\n",
      "classname", name_resolver_->GetImmutableClassName(descriptor_));

  printer->Print(
      "@java.lang.Override\n"
      "public $classname$ build() {\n"
      "  $classname$ result = buildPartial();\n"
      "  if (!result.isInitialized()) {\n"
      "    throw newUninitializedMessageException(result);\n"
      "  }\n"
      "  return result;\n"
      "}\n"
      "\n",
      "classname", name_resolver_->GetImmutableClassName(descriptor_));

  printer->Print(
      "@java.lang.Override\n"
      "public $classname$ buildPartial() {\n"
      "  $classname$ result = new $classname$(this);\n",
      "classname", name_resolver_->GetImmutableClassName(descriptor_));

  printer->Indent();

  int totalBuilderBits = 0;
  int totalMessageBits = 0;
  for (int i = 0; i < descriptor_->field_count(); i++) {
    const ImmutableFieldGenerator& field =
        field_generators_.get(descriptor_->field(i));
    totalBuilderBits += field.GetNumBitsForBuilder();
    totalMessageBits += field.GetNumBitsForMessage();
  }
  int totalBuilderInts = (totalBuilderBits + 31) / 32;
  int totalMessageInts = (totalMessageBits + 31) / 32;

  // Local vars for from and to bit fields to avoid accessing the builder and
  // message over and over for these fields. Seems to provide a slight
  // perforamance improvement in micro benchmark and this is also what proto1
  // code does.
  for (int i = 0; i < totalBuilderInts; i++) {
    printer->Print("int from_$bit_field_name$ = $bit_field_name$;\n",
                   "bit_field_name", GetBitFieldName(i));
  }
  for (int i = 0; i < totalMessageInts; i++) {
    printer->Print("int to_$bit_field_name$ = 0;\n", "bit_field_name",
                   GetBitFieldName(i));
  }

  // Output generation code for each field.
  for (int i = 0; i < descriptor_->field_count(); i++) {
    field_generators_.get(descriptor_->field(i)).GenerateBuildingCode(printer);
  }

  // Copy the bit field results to the generated message
  for (int i = 0; i < totalMessageInts; i++) {
    printer->Print("result.$bit_field_name$ = to_$bit_field_name$;\n",
                   "bit_field_name", GetBitFieldName(i));
  }

  for (auto oneof : oneofs_) {
    printer->Print("result.$oneof_name$Case_ = $oneof_name$Case_;\n",
                   "oneof_name", context_->GetOneofGeneratorInfo(oneof)->name);
  }

  printer->Outdent();

  printer->Print("  onBuilt();\n");

  printer->Print(
      "  return result;\n"
      "}\n"
      "\n",
      "classname", name_resolver_->GetImmutableClassName(descriptor_));

  if (context_->options().opensource_runtime) {
    // Override methods declared in GeneratedMessage to return the concrete
    // generated type so callsites won't depend on GeneratedMessage. This
    // is needed to keep binary compatibility when we change generated code
    // to subclass a different GeneratedMessage class (e.g., in v3.0.0 release
    // we changed all generated code to subclass GeneratedMessageV3).
    printer->Print(
        "@java.lang.Override\n"
        "public Builder clone() {\n"
        "  return super.clone();\n"
        "}\n"
        "@java.lang.Override\n"
        "public Builder setField(\n"
        "    com.google.protobuf.Descriptors.FieldDescriptor field,\n"
        "    java.lang.Object value) {\n"
        "  return super.setField(field, value);\n"
        "}\n"
        "@java.lang.Override\n"
        "public Builder clearField(\n"
        "    com.google.protobuf.Descriptors.FieldDescriptor field) {\n"
        "  return super.clearField(field);\n"
        "}\n"
        "@java.lang.Override\n"
        "public Builder clearOneof(\n"
        "    com.google.protobuf.Descriptors.OneofDescriptor oneof) {\n"
        "  return super.clearOneof(oneof);\n"
        "}\n"
        "@java.lang.Override\n"
        "public Builder setRepeatedField(\n"
        "    com.google.protobuf.Descriptors.FieldDescriptor field,\n"
        "    int index, java.lang.Object value) {\n"
        "  return super.setRepeatedField(field, index, value);\n"
        "}\n"
        "@java.lang.Override\n"
        "public Builder addRepeatedField(\n"
        "    com.google.protobuf.Descriptors.FieldDescriptor field,\n"
        "    java.lang.Object value) {\n"
        "  return super.addRepeatedField(field, value);\n"
        "}\n");

    if (descriptor_->extension_range_count() > 0) {
      printer->Print(
          "@java.lang.Override\n"
          "public <Type> Builder setExtension(\n"
          "    com.google.protobuf.GeneratedMessage.GeneratedExtension<\n"
          "        $classname$, Type> extension,\n"
          "    Type value) {\n"
          "  return super.setExtension(extension, value);\n"
          "}\n"
          "@java.lang.Override\n"
          "public <Type> Builder setExtension(\n"
          "    com.google.protobuf.GeneratedMessage.GeneratedExtension<\n"
          "        $classname$, java.util.List<Type>> extension,\n"
          "    int index, Type value) {\n"
          "  return super.setExtension(extension, index, value);\n"
          "}\n"
          "@java.lang.Override\n"
          "public <Type> Builder addExtension(\n"
          "    com.google.protobuf.GeneratedMessage.GeneratedExtension<\n"
          "        $classname$, java.util.List<Type>> extension,\n"
          "    Type value) {\n"
          "  return super.addExtension(extension, value);\n"
          "}\n"
          "@java.lang.Override\n"
          "public <Type> Builder clearExtension(\n"
          "    com.google.protobuf.GeneratedMessage.GeneratedExtension<\n"
          "        $classname$, ?> extension) {\n"
          "  return super.clearExtension(extension);\n"
          "}\n",
          "classname", name_resolver_->GetImmutableClassName(descriptor_));
    }
  }

  // -----------------------------------------------------------------

  if (context_->HasGeneratedMethods(descriptor_)) {
    printer->Print(
        "@java.lang.Override\n"
        "public Builder mergeFrom(com.google.protobuf.Message other) {\n"
        "  if (other instanceof $classname$) {\n"
        "    return mergeFrom(($classname$)other);\n"
        "  } else {\n"
        "    super.mergeFrom(other);\n"
        "    return this;\n"
        "  }\n"
        "}\n"
        "\n",
        "classname", name_resolver_->GetImmutableClassName(descriptor_));

    printer->Print(
        "public Builder mergeFrom($classname$ other) {\n"
        // Optimization:  If other is the default instance, we know none of its
        //   fields are set so we can skip the merge.
        "  if (other == $classname$.getDefaultInstance()) return this;\n",
        "classname", name_resolver_->GetImmutableClassName(descriptor_));
    printer->Indent();

    for (int i = 0; i < descriptor_->field_count(); i++) {
      if (!IsRealOneof(descriptor_->field(i))) {
        field_generators_.get(descriptor_->field(i))
            .GenerateMergingCode(printer);
      }
    }

    // Merge oneof fields.
    for (auto oneof : oneofs_) {
      printer->Print("switch (other.get$oneof_capitalized_name$Case()) {\n",
                     "oneof_capitalized_name",
                     context_->GetOneofGeneratorInfo(oneof)->capitalized_name);
      printer->Indent();
      for (int j = 0; j < oneof->field_count(); j++) {
        const FieldDescriptor* field = oneof->field(j);
        printer->Print("case $field_name$: {\n", "field_name",
                       absl::AsciiStrToUpper(field->name()));
        printer->Indent();
        field_generators_.get(field).GenerateMergingCode(printer);
        printer->Print("break;\n");
        printer->Outdent();
        printer->Print("}\n");
      }
      printer->Print(
          "case $cap_oneof_name$_NOT_SET: {\n"
          "  break;\n"
          "}\n",
          "cap_oneof_name",
          absl::AsciiStrToUpper(context_->GetOneofGeneratorInfo(oneof)->name));
      printer->Outdent();
      printer->Print("}\n");
    }

    printer->Outdent();

    // if message type has extensions
    if (descriptor_->extension_range_count() > 0) {
      printer->Print("  this.mergeExtensionFields(other);\n");
    }

    printer->Print("  this.mergeUnknownFields(other.getUnknownFields());\n");

    printer->Print("  onChanged();\n");

    printer->Print(
        "  return this;\n"
        "}\n"
        "\n");
  }
}

// ===================================================================

void MessageBuilderGenerator::GenerateBuilderParsingMethods(
    io::Printer* printer) {
  printer->Print(
      "@java.lang.Override\n"
      "public Builder mergeFrom(\n"
      "    com.google.protobuf.CodedInputStream input,\n"
      "    com.google.protobuf.ExtensionRegistryLite extensionRegistry)\n"
      "    throws java.io.IOException {\n"
      "  if (extensionRegistry == null) {\n"
      "    throw new java.lang.NullPointerException();\n"
      "  }\n"
      "  try {\n"
      "    boolean done = false;\n"
      "    while (!done) {\n"
      "      int tag = input.readTag();\n"
      "      switch (tag) {\n"
      "        case 0:\n"  // zero signals EOF / limit reached
      "          done = true;\n"
      "          break;\n");
  printer->Indent();  // method
  printer->Indent();  // try
  printer->Indent();  // while
  printer->Indent();  // switch
  GenerateBuilderFieldParsingCases(printer);
  printer->Outdent();  // switch
  printer->Outdent();  // while
  printer->Outdent();  // try
  printer->Outdent();  // method
  printer->Print(
      "        default: {\n"
      "          if (!super.parseUnknownField(input, extensionRegistry, tag)) "
      "{\n"
      "            done = true; // was an endgroup tag\n"
      "          }\n"
      "          break;\n"
      "        } // default:\n"
      "      } // switch (tag)\n"
      "    } // while (!done)\n"
      "  } catch (com.google.protobuf.InvalidProtocolBufferException e) {\n"
      "    throw e.unwrapIOException();\n"
      "  } finally {\n"
      "    onChanged();\n"
      "  } // finally\n"
      "  return this;\n"
      "}\n");
}

void MessageBuilderGenerator::GenerateBuilderFieldParsingCases(
    io::Printer* printer) {
  std::unique_ptr<const FieldDescriptor*[]> sorted_fields(
      SortFieldsByNumber(descriptor_));
  for (int i = 0; i < descriptor_->field_count(); i++) {
    const FieldDescriptor* field = sorted_fields[i];
    GenerateBuilderFieldParsingCase(printer, field);
    if (field->is_packable()) {
      GenerateBuilderPackedFieldParsingCase(printer, field);
    }
  }
}

void MessageBuilderGenerator::GenerateBuilderFieldParsingCase(
    io::Printer* printer, const FieldDescriptor* field) {
  uint32_t tag = WireFormatLite::MakeTag(
      field->number(), WireFormat::WireTypeForFieldType(field->type()));
  std::string tagString = absl::StrCat(static_cast<int32_t>(tag));
  printer->Print("case $tag$: {\n", "tag", tagString);
  printer->Indent();

  field_generators_.get(field).GenerateBuilderParsingCode(printer);

  printer->Outdent();
  printer->Print(
      "  break;\n"
      "} // case $tag$\n",
      "tag", tagString);
}

void MessageBuilderGenerator::GenerateBuilderPackedFieldParsingCase(
    io::Printer* printer, const FieldDescriptor* field) {
  // To make packed = true wire compatible, we generate parsing code from a
  // packed version of this field regardless of field->options().packed().
  uint32_t tag = WireFormatLite::MakeTag(
      field->number(), WireFormatLite::WIRETYPE_LENGTH_DELIMITED);
  std::string tagString = absl::StrCat(static_cast<int32_t>(tag));
  printer->Print("case $tag$: {\n", "tag", tagString);
  printer->Indent();

  field_generators_.get(field).GenerateBuilderParsingCodeFromPacked(printer);

  printer->Outdent();
  printer->Print(
      "  break;\n"
      "} // case $tag$\n",
      "tag", tagString);
}

// ===================================================================

void MessageBuilderGenerator::GenerateIsInitialized(io::Printer* printer) {
  printer->Print(
      "@java.lang.Override\n"
      "public final boolean isInitialized() {\n");
  printer->Indent();

  // Check that all required fields in this message are set.
  // TODO(kenton):  We can optimize this when we switch to putting all the
  //   "has" fields into a single bitfield.
  for (int i = 0; i < descriptor_->field_count(); i++) {
    const FieldDescriptor* field = descriptor_->field(i);
    const FieldGeneratorInfo* info = context_->GetFieldGeneratorInfo(field);

    if (field->is_required()) {
      printer->Print(
          "if (!has$name$()) {\n"
          "  return false;\n"
          "}\n",
          "name", info->capitalized_name);
    }
  }

  // Now check that all embedded messages are initialized.
  for (int i = 0; i < descriptor_->field_count(); i++) {
    const FieldDescriptor* field = descriptor_->field(i);
    const FieldGeneratorInfo* info = context_->GetFieldGeneratorInfo(field);
    if (GetJavaType(field) == JAVATYPE_MESSAGE &&
        HasRequiredFields(field->message_type())) {
      switch (field->label()) {
        case FieldDescriptor::LABEL_REQUIRED:
          printer->Print(
              "if (!get$name$().isInitialized()) {\n"
              "  return false;\n"
              "}\n",
              "type",
              name_resolver_->GetImmutableClassName(field->message_type()),
              "name", info->capitalized_name);
          break;
        case FieldDescriptor::LABEL_OPTIONAL:
          printer->Print(
              "if (has$name$()) {\n"
              "  if (!get$name$().isInitialized()) {\n"
              "    return false;\n"
              "  }\n"
              "}\n",
              "name", info->capitalized_name);
          break;
        case FieldDescriptor::LABEL_REPEATED:
          if (IsMapEntry(field->message_type())) {
            printer->Print(
                "for ($type$ item : get$name$Map().values()) {\n"
                "  if (!item.isInitialized()) {\n"
                "    return false;\n"
                "  }\n"
                "}\n",
                "type",
                MapValueImmutableClassdName(field->message_type(),
                                            name_resolver_),
                "name", info->capitalized_name);
          } else {
            printer->Print(
                "for (int i = 0; i < get$name$Count(); i++) {\n"
                "  if (!get$name$(i).isInitialized()) {\n"
                "    return false;\n"
                "  }\n"
                "}\n",
                "type",
                name_resolver_->GetImmutableClassName(field->message_type()),
                "name", info->capitalized_name);
          }
          break;
      }
    }
  }

  if (descriptor_->extension_range_count() > 0) {
    printer->Print(
        "if (!extensionsAreInitialized()) {\n"
        "  return false;\n"
        "}\n");
  }

  printer->Outdent();

  printer->Print(
      "  return true;\n"
      "}\n"
      "\n");
}

// ===================================================================

}  // namespace java
}  // namespace compiler
}  // namespace protobuf
}  // namespace google

#include "google/protobuf/port_undef.inc"
