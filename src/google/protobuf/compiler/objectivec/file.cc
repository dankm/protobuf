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

#include "google/protobuf/compiler/objectivec/file.h"

#include <algorithm>
#include <iostream>
#include <iterator>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "google/protobuf/compiler/code_generator.h"
#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/strings/str_cat.h"
#include "google/protobuf/compiler/objectivec/enum.h"
#include "google/protobuf/compiler/objectivec/extension.h"
#include "google/protobuf/compiler/objectivec/import_writer.h"
#include "google/protobuf/compiler/objectivec/message.h"
#include "google/protobuf/compiler/objectivec/names.h"
#include "google/protobuf/io/printer.h"

// NOTE: src/google/protobuf/compiler/plugin.cc makes use of cerr for some
// error cases, so it seems to be ok to use as a back door for errors.

namespace google {
namespace protobuf {
namespace compiler {
namespace objectivec {

namespace {

// This is also found in GPBBootstrap.h, and needs to be kept in sync.
const int32_t GOOGLE_PROTOBUF_OBJC_VERSION = 30004;

const char* kHeaderExtension = ".pbobjc.h";

std::string BundledFileName(const FileDescriptor* file) {
  return "GPB" + FilePathBasename(file) + kHeaderExtension;
}

// Checks if a message contains any enums definitions (on the message or
// a nested message under it).
bool MessageContainsEnums(const Descriptor* message) {
  if (message->enum_type_count() > 0) {
    return true;
  }
  for (int i = 0; i < message->nested_type_count(); i++) {
    if (MessageContainsEnums(message->nested_type(i))) {
      return true;
    }
  }
  return false;
}

// Checks if a message contains any extension definitions (on the message or
// a nested message under it).
bool MessageContainsExtensions(const Descriptor* message) {
  if (message->extension_count() > 0) {
    return true;
  }
  for (int i = 0; i < message->nested_type_count(); i++) {
    if (MessageContainsExtensions(message->nested_type(i))) {
      return true;
    }
  }
  return false;
}

// Checks if the file contains any enum definitions (at the root or
// nested under a message).
bool FileContainsEnums(const FileDescriptor* file) {
  if (file->enum_type_count() > 0) {
    return true;
  }
  for (int i = 0; i < file->message_type_count(); i++) {
    if (MessageContainsEnums(file->message_type(i))) {
      return true;
    }
  }
  return false;
}

// Checks if the file contains any extensions definitions (at the root or
// nested under a message).
bool FileContainsExtensions(const FileDescriptor* file) {
  if (file->extension_count() > 0) {
    return true;
  }
  for (int i = 0; i < file->message_type_count(); i++) {
    if (MessageContainsExtensions(file->message_type(i))) {
      return true;
    }
  }
  return false;
}

bool IsDirectDependency(const FileDescriptor* dep, const FileDescriptor* file) {
  for (int i = 0; i < file->dependency_count(); i++) {
    if (dep == file->dependency(i)) {
      return true;
    }
  }
  return false;
}

struct FileDescriptorsOrderedByName {
  inline bool operator()(const FileDescriptor* a,
                         const FileDescriptor* b) const {
    return a->name() < b->name();
  }
};

}  // namespace

const FileGenerator::CommonState::MinDepsEntry&
FileGenerator::CommonState::CollectMinimalFileDepsContainingExtensionsInternal(
    const FileDescriptor* file) {
  auto it = deps_info_cache.find(file);
  if (it != deps_info_cache.end()) {
    return it->second;
  }

  absl::flat_hash_set<const FileDescriptor*> min_deps_collector;
  absl::flat_hash_set<const FileDescriptor*> covered_deps_collector;
  absl::flat_hash_set<const FileDescriptor*> to_prune;
  for (int i = 0; i < file->dependency_count(); i++) {
    const FileDescriptor* dep = file->dependency(i);
    MinDepsEntry dep_info =
        CollectMinimalFileDepsContainingExtensionsInternal(dep);

    // Everything the dep covered, this file will also cover.
    covered_deps_collector.insert(dep_info.covered_deps.begin(),
                                  dep_info.covered_deps.end());
    // Prune everything from the dep's covered list in case another dep lists it
    // as a min dep.
    to_prune.insert(dep_info.covered_deps.begin(), dep_info.covered_deps.end());

    // Does the dep have any extensions...
    if (dep_info.has_extensions) {
      // Yes -> Add this file, prune its min_deps and add them to the covered
      // deps.
      min_deps_collector.insert(dep);
      to_prune.insert(dep_info.min_deps.begin(), dep_info.min_deps.end());
      covered_deps_collector.insert(dep_info.min_deps.begin(),
                                    dep_info.min_deps.end());
    } else {
      // No -> Just use its min_deps.
      min_deps_collector.insert(dep_info.min_deps.begin(),
                                dep_info.min_deps.end());
    }
  }

  const bool file_has_exts = FileContainsExtensions(file);

  // Fast path: if nothing to prune or there was only one dep, the prune work is
  // a waste, skip it.
  if (to_prune.empty() || file->dependency_count() == 1) {
    return deps_info_cache
        .insert(
            {file, {file_has_exts, min_deps_collector, covered_deps_collector}})
        .first->second;
  }

  absl::flat_hash_set<const FileDescriptor*> min_deps;
  std::copy_if(min_deps_collector.begin(), min_deps_collector.end(),
               std::inserter(min_deps, min_deps.end()),
               [&](const FileDescriptor* value) {
                 return to_prune.find(value) == to_prune.end();
               });
  return deps_info_cache
      .insert({file, {file_has_exts, min_deps, covered_deps_collector}})
      .first->second;
}

// Collect the deps of the given file that contain extensions. This can be used
// to create the chain of roots that need to be wired together.
//
// NOTE: If any changes are made to this and the supporting functions, you will
// need to manually validate what the generated code is for the test files:
//   objectivec/Tests/unittest_extension_chain_*.proto
// There are comments about what the expected code should be line and limited
// testing objectivec/Tests/GPBUnittestProtos2.m around compilation (#imports
// specifically).
const std::vector<const FileDescriptor*>
FileGenerator::CommonState::CollectMinimalFileDepsContainingExtensions(
    const FileDescriptor* file) {
  absl::flat_hash_set<const FileDescriptor*> min_deps =
      CollectMinimalFileDepsContainingExtensionsInternal(file).min_deps;
  // Sort the list since pointer order isn't stable across runs.
  std::vector<const FileDescriptor*> result(min_deps.begin(), min_deps.end());
  std::sort(result.begin(), result.end(), FileDescriptorsOrderedByName());
  return result;
}

FileGenerator::FileGenerator(const FileDescriptor* file,
                             const GenerationOptions& generation_options,
                             CommonState& common_state)
    : file_(file),
      generation_options_(generation_options),
      common_state_(common_state),
      root_class_name_(FileClassName(file)),
      is_bundled_proto_(IsProtobufLibraryBundledProtoFile(file)) {
  for (int i = 0; i < file_->enum_type_count(); i++) {
    EnumGenerator* generator = new EnumGenerator(file_->enum_type(i));
    enum_generators_.emplace_back(generator);
  }
  for (int i = 0; i < file_->message_type_count(); i++) {
    MessageGenerator* generator =
        new MessageGenerator(root_class_name_, file_->message_type(i));
    message_generators_.emplace_back(generator);
  }
  for (int i = 0; i < file_->extension_count(); i++) {
    ExtensionGenerator* generator =
        new ExtensionGenerator(root_class_name_, file_->extension(i));
    extension_generators_.emplace_back(generator);
  }
}

void FileGenerator::GenerateHeader(io::Printer* printer) {
  std::vector<std::string> headers;
  // Generated files bundled with the library get minimal imports, everything
  // else gets the wrapper so everything is usable.
  if (is_bundled_proto_) {
    headers.push_back("GPBDescriptor.h");
    headers.push_back("GPBMessage.h");
    headers.push_back("GPBRootObject.h");
    for (int i = 0; i < file_->dependency_count(); i++) {
      const std::string header_name = BundledFileName(file_->dependency(i));
      headers.push_back(header_name);
    }
  } else {
    headers.push_back("GPBProtocolBuffers.h");
  }
  PrintFileRuntimePreamble(printer, headers);

  // Add some verification that the generated code matches the source the
  // code is being compiled with.
  // NOTE: This captures the raw numeric values at the time the generator was
  // compiled, since that will be the versions for the ObjC runtime at that
  // time.  The constants in the generated code will then get their values at
  // at compile time (so checking against the headers being used to compile).
  // clang-format off
  printer->Print(
      "#if GOOGLE_PROTOBUF_OBJC_VERSION < $google_protobuf_objc_version$\n"
      "#error This file was generated by a newer version of protoc which is incompatible with your Protocol Buffer library sources.\n"
      "#endif\n"
      "#if $google_protobuf_objc_version$ < GOOGLE_PROTOBUF_OBJC_MIN_SUPPORTED_VERSION\n"
      "#error This file was generated by an older version of protoc which is incompatible with your Protocol Buffer library sources.\n"
      "#endif\n"
      "\n",
      "google_protobuf_objc_version", absl::StrCat(GOOGLE_PROTOBUF_OBJC_VERSION));
  // clang-format on

  // The bundled protos (WKTs) don't use of forward declarations.
  bool headers_use_forward_declarations =
      generation_options_.headers_use_forward_declarations &&
      !is_bundled_proto_;

  {
    ImportWriter import_writer(
        generation_options_.generate_for_named_framework,
        generation_options_.named_framework_to_proto_path_mappings_path,
        generation_options_.runtime_import_prefix,
        /* include_wkt_imports = */ false);
    const std::string header_extension(kHeaderExtension);
    if (headers_use_forward_declarations) {
      // #import any headers for "public imports" in the proto file.
      for (int i = 0; i < file_->public_dependency_count(); i++) {
        import_writer.AddFile(file_->public_dependency(i), header_extension);
      }
    } else {
      for (int i = 0; i < file_->dependency_count(); i++) {
        import_writer.AddFile(file_->dependency(i), header_extension);
      }
    }
    import_writer.Print(printer);
  }

  // Note:
  //  deprecated-declarations suppression is only needed if some place in this
  //    proto file is something deprecated or if it references something from
  //    another file that is deprecated.
  // clang-format off
  printer->Print(
      "// @@protoc_insertion_point(imports)\n"
      "\n"
      "#pragma clang diagnostic push\n"
      "#pragma clang diagnostic ignored \"-Wdeprecated-declarations\"\n"
      "\n"
      "CF_EXTERN_C_BEGIN\n"
      "\n");
  // clang-format on

  std::set<std::string> fwd_decls;
  for (const auto& generator : message_generators_) {
    generator->DetermineForwardDeclarations(
        &fwd_decls,
        /* include_external_types = */ headers_use_forward_declarations);
  }
  for (std::set<std::string>::const_iterator i(fwd_decls.begin());
       i != fwd_decls.end(); ++i) {
    printer->Print("$value$;\n", "value", *i);
  }
  if (fwd_decls.begin() != fwd_decls.end()) {
    printer->Print("\n");
  }

  printer->Print(
      "NS_ASSUME_NONNULL_BEGIN\n"
      "\n");

  // need to write out all enums first
  for (const auto& generator : enum_generators_) {
    generator->GenerateHeader(printer);
  }

  for (const auto& generator : message_generators_) {
    generator->GenerateEnumHeader(printer);
  }

  // For extensions to chain together, the Root gets created even if there
  // are no extensions.
  printer->Print(
      // clang-format off
      "#pragma mark - $root_class_name$\n"
      "\n"
      "/**\n"
      " * Exposes the extension registry for this file.\n"
      " *\n"
      " * The base class provides:\n"
      " * @code\n"
      " *   + (GPBExtensionRegistry *)extensionRegistry;\n"
      " * @endcode\n"
      " * which is a @c GPBExtensionRegistry that includes all the extensions defined by\n"
      " * this file and all files that it depends on.\n"
      " **/\n"
      "GPB_FINAL @interface $root_class_name$ : GPBRootObject\n"
      "@end\n"
      "\n",
      // clang-format off
      "root_class_name", root_class_name_);

  if (!extension_generators_.empty()) {
    // The dynamic methods block is only needed if there are extensions.
    printer->Print(
        "@interface $root_class_name$ (DynamicMethods)\n",
        "root_class_name", root_class_name_);

    for (const auto& generator : extension_generators_) {
      generator->GenerateMembersHeader(printer);
    }

    printer->Print("@end\n\n");
  }  // !extension_generators_.empty()

  for (const auto& generator : message_generators_) {
    generator->GenerateMessageHeader(printer);
  }

  // clang-format off
  printer->Print(
      "NS_ASSUME_NONNULL_END\n"
      "\n"
      "CF_EXTERN_C_END\n"
      "\n"
      "#pragma clang diagnostic pop\n"
      "\n"
      "// @@protoc_insertion_point(global_scope)\n"
      "\n"
      "// clange-format on\n");
  // clang-format on
}

void FileGenerator::GenerateSource(io::Printer* printer) {
  // #import the runtime support.
  std::vector<std::string> headers;
  headers.push_back("GPBProtocolBuffers_RuntimeSupport.h");
  if (is_bundled_proto_) {
    headers.push_back(BundledFileName(file_));
  }
  PrintFileRuntimePreamble(printer, headers);

  // Enums use atomic in the generated code, so add the system import as needed.
  if (FileContainsEnums(file_)) {
    printer->Print(
        "#import <stdatomic.h>\n"
        "\n");
  }

  std::vector<const FileDescriptor*> deps_with_extensions =
      common_state_.CollectMinimalFileDepsContainingExtensions(file_);

  // The bundled protos (WKTs) don't use of forward declarations.
  bool headers_use_forward_declarations =
      generation_options_.headers_use_forward_declarations &&
      !is_bundled_proto_;

  {
    ImportWriter import_writer(
        generation_options_.generate_for_named_framework,
        generation_options_.named_framework_to_proto_path_mappings_path,
        generation_options_.runtime_import_prefix,
        /* include_wkt_imports = */ false);
    const std::string header_extension(kHeaderExtension);

    // #import the header for this proto file.
    import_writer.AddFile(file_, header_extension);

    if (headers_use_forward_declarations) {
      // #import the headers for anything that a plain dependency of this proto
      // file (that means they were just an include, not a "public" include).
      std::set<std::string> public_import_names;
      for (int i = 0; i < file_->public_dependency_count(); i++) {
        public_import_names.insert(file_->public_dependency(i)->name());
      }
      for (int i = 0; i < file_->dependency_count(); i++) {
        const FileDescriptor* dep = file_->dependency(i);
        bool public_import = (public_import_names.count(dep->name()) != 0);
        if (!public_import) {
          import_writer.AddFile(dep, header_extension);
        }
      }
    }

    // If any indirect dependency provided extensions, it needs to be directly
    // imported so it can get merged into the root's extensions registry.
    // See the Note by CollectMinimalFileDepsContainingExtensions before
    // changing this.
    for (std::vector<const FileDescriptor*>::iterator iter =
             deps_with_extensions.begin();
         iter != deps_with_extensions.end(); ++iter) {
      if (!IsDirectDependency(*iter, file_)) {
        import_writer.AddFile(*iter, header_extension);
      }
    }

    import_writer.Print(printer);
  }

  bool includes_oneof = false;
  for (const auto& generator : message_generators_) {
    if (generator->IncludesOneOfDefinition()) {
      includes_oneof = true;
      break;
    }
  }

  std::set<std::string> fwd_decls;
  for (const auto& generator : message_generators_) {
    generator->DetermineObjectiveCClassDefinitions(&fwd_decls);
  }
  for (const auto& generator : extension_generators_) {
    generator->DetermineObjectiveCClassDefinitions(&fwd_decls);
  }

  // Note:
  //  deprecated-declarations suppression is only needed if some place in this
  //    proto file is something deprecated or if it references something from
  //    another file that is deprecated.
  //  dollar-in-identifier-extension is needed because we use references to
  //    objc class names that have $ in identifiers.
  // clang-format off
  printer->Print(
      "// @@protoc_insertion_point(imports)\n"
      "\n"
      "#pragma clang diagnostic push\n"
      "#pragma clang diagnostic ignored \"-Wdeprecated-declarations\"\n");
  // clang-format on
  if (includes_oneof) {
    // The generated code for oneof's uses direct ivar access, suppress the
    // warning in case developer turn that on in the context they compile the
    // generated code.
    printer->Print(
        "#pragma clang diagnostic ignored \"-Wdirect-ivar-access\"\n");
  }
  if (!fwd_decls.empty()) {
    // clang-format off
    printer->Print(
        "#pragma clang diagnostic ignored \"-Wdollar-in-identifier-extension\"\n");
    // clang-format on
  }
  printer->Print("\n");
  if (!fwd_decls.empty()) {
    // clang-format off
    printer->Print(
        "#pragma mark - Objective C Class declarations\n"
        "// Forward declarations of Objective C classes that we can use as\n"
        "// static values in struct initializers.\n"
        "// We don't use [Foo class] because it is not a static value.\n");
    // clang-format on
  }
  for (const auto& i : fwd_decls) {
    printer->Print("$value$\n", "value", i);
  }
  if (!fwd_decls.empty()) {
    printer->Print("\n");
  }
  printer->Print(
      // clang-format off
      "#pragma mark - $root_class_name$\n"
      "\n"
      "@implementation $root_class_name$\n\n",
      // clang-format on
      "root_class_name", root_class_name_);

  const bool file_contains_extensions = FileContainsExtensions(file_);

  // If there were any extensions or this file has any dependencies, output
  // a registry to override to create the file specific registry.
  if (file_contains_extensions || !deps_with_extensions.empty()) {
    // clang-format off
    printer->Print(
        "+ (GPBExtensionRegistry*)extensionRegistry {\n"
        "  // This is called by +initialize so there is no need to worry\n"
        "  // about thread safety and initialization of registry.\n"
        "  static GPBExtensionRegistry* registry = nil;\n"
        "  if (!registry) {\n"
        "    GPB_DEBUG_CHECK_RUNTIME_VERSIONS();\n"
        "    registry = [[GPBExtensionRegistry alloc] init];\n");
    // clang-format on

    printer->Indent();
    printer->Indent();

    if (file_contains_extensions) {
      printer->Print("static GPBExtensionDescription descriptions[] = {\n");
      printer->Indent();
      for (const auto& generator : extension_generators_) {
        generator->GenerateStaticVariablesInitialization(printer);
      }
      for (const auto& generator : message_generators_) {
        generator->GenerateStaticVariablesInitialization(printer);
      }
      printer->Outdent();
      // clang-format off
      printer->Print(
          "};\n"
          "for (size_t i = 0; i < sizeof(descriptions) / sizeof(descriptions[0]); ++i) {\n"
          "  GPBExtensionDescriptor *extension =\n"
          "      [[GPBExtensionDescriptor alloc] initWithExtensionDescription:&descriptions[i]\n"
          "                                                     usesClassRefs:YES];\n"
          "  [registry addExtension:extension];\n"
          "  [self globallyRegisterExtension:extension];\n"
          "  [extension release];\n"
          "}\n");
      // clang-format on
    }

    if (deps_with_extensions.empty()) {
      // clang-format off
      printer->Print(
          "// None of the imports (direct or indirect) defined extensions, so no need to add\n"
          "// them to this registry.\n");
      // clang-format on
    } else {
      // clang-format off
      printer->Print(
          "// Merge in the imports (direct or indirect) that defined extensions.\n");
      // clang-format on
      for (std::vector<const FileDescriptor*>::iterator iter =
               deps_with_extensions.begin();
           iter != deps_with_extensions.end(); ++iter) {
        const std::string root_class_name(FileClassName((*iter)));
        printer->Print(
            "[registry addExtensions:[$dependency$ extensionRegistry]];\n",
            "dependency", root_class_name);
      }
    }

    printer->Outdent();
    printer->Outdent();

    // clang-format off
    printer->Print(
        "  }\n"
        "  return registry;\n"
        "}\n");
    // clang-format on
  } else {
    if (file_->dependency_count() > 0) {
      // clang-format off
      printer->Print(
          "// No extensions in the file and none of the imports (direct or indirect)\n"
          "// defined extensions, so no need to generate +extensionRegistry.\n");
      // clang-format on
    } else {
      // clang-format off
      printer->Print(
          "// No extensions in the file and no imports, so no need to generate\n"
          "// +extensionRegistry.\n");
      // clang-format on
    }
  }

  printer->Print("\n@end\n\n");

  // File descriptor only needed if there are messages to use it.
  if (!message_generators_.empty()) {
    absl::flat_hash_map<absl::string_view, std::string> vars;
    vars["root_class_name"] = root_class_name_;
    vars["package"] = file_->package();
    vars["objc_prefix"] = FileClassPrefix(file_);
    switch (file_->syntax()) {
      case FileDescriptor::SYNTAX_UNKNOWN:
        vars["syntax"] = "GPBFileSyntaxUnknown";
        break;
      case FileDescriptor::SYNTAX_PROTO2:
        vars["syntax"] = "GPBFileSyntaxProto2";
        break;
      case FileDescriptor::SYNTAX_PROTO3:
        vars["syntax"] = "GPBFileSyntaxProto3";
        break;
    }
    // clang-format off
    printer->Print(
        vars,
        "#pragma mark - $root_class_name$_FileDescriptor\n"
        "\n"
        "static GPBFileDescriptor *$root_class_name$_FileDescriptor(void) {\n"
        "  // This is called by +initialize so there is no need to worry\n"
        "  // about thread safety of the singleton.\n"
        "  static GPBFileDescriptor *descriptor = NULL;\n"
        "  if (!descriptor) {\n"
        "    GPB_DEBUG_CHECK_RUNTIME_VERSIONS();\n");
    // clang-format on
    if (!vars["objc_prefix"].empty()) {
      // clang-format off
      printer->Print(
          vars,
          "    descriptor = [[GPBFileDescriptor alloc] initWithPackage:@\"$package$\"\n"
          "                                                 objcPrefix:@\"$objc_prefix$\"\n"
          "                                                     syntax:$syntax$];\n");
      // clang-format on
    } else {
      // clang-format off
      printer->Print(
          vars,
          "    descriptor = [[GPBFileDescriptor alloc] initWithPackage:@\"$package$\"\n"
          "                                                     syntax:$syntax$];\n");
      // clang-format on
    }
    // clang-format off
    printer->Print(
        "  }\n"
        "  return descriptor;\n"
        "}\n"
        "\n");
    // clang-format on
  }

  for (const auto& generator : enum_generators_) {
    generator->GenerateSource(printer);
  }
  for (const auto& generator : message_generators_) {
    generator->GenerateSource(printer);
  }

  // clang-format off
  printer->Print(
      "\n"
      "#pragma clang diagnostic pop\n"
      "\n"
      "// @@protoc_insertion_point(global_scope)\n"
      "\n"
      "// clang-format on\n");
  // clang-format on
}

// Helper to print the import of the runtime support at the top of generated
// files. This currently only supports the runtime coming from a framework
// as defined by the official CocoaPod.
void FileGenerator::PrintFileRuntimePreamble(
    io::Printer* printer,
    const std::vector<std::string>& headers_to_import) const {
  printer->Print(
      "// Generated by the protocol buffer compiler.  DO NOT EDIT!\n"
      "// clang-format off\n"
      "// source: $filename$\n"
      "\n",
      "filename", file_->name());

  if (is_bundled_proto_) {
    // This is basically a clone of ImportWriter::PrintRuntimeImports() but
    // without the CPP symbol gate, since within the bundled files, that isn't
    // needed.
    std::string import_prefix = generation_options_.runtime_import_prefix;
    if (!import_prefix.empty()) {
      import_prefix += "/";
    }
    for (const auto& header : headers_to_import) {
      printer->Print("#import \"$import_prefix$$header$\"\n", "import_prefix",
                     import_prefix, "header", header);
    }
  } else {
    ImportWriter::PrintRuntimeImports(printer, headers_to_import,
                                      generation_options_.runtime_import_prefix,
                                      true);
  }

  printer->Print("\n");
}

}  // namespace objectivec
}  // namespace compiler
}  // namespace protobuf
}  // namespace google
