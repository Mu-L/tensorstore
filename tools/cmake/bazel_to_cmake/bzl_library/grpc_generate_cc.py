# Copyright 2022 The TensorStore Authors
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
"""CMake implementation of "@tensorstore//bazel:cc_grpc_library.bzl"."""

# pylint: disable=invalid-name,missing-function-docstring,relative-beyond-top-level,g-long-lambda
import io
from typing import Any, List, Optional, cast

from ..cmake_builder import CMakeBuilder
from ..cmake_provider import CMakeAddDependenciesProvider
from ..cmake_provider import CMakePackageDepsProvider
from ..cmake_provider import make_providers
from ..evaluation import EvaluationState
from ..native_aspect_proto import btc_protobuf
from ..native_aspect_proto import plugin_generated_files
from ..native_aspect_proto import PluginSettings
from ..native_aspect_proto import maybe_augment_output_dir
from ..starlark.scope_common import ScopeCommon
from ..starlark.bazel_target import RepositoryId
from ..starlark.bazel_target import TargetId
from ..starlark.common_providers import FilesProvider
from ..starlark.common_providers import ProtoLibraryProvider
from ..starlark.invocation_context import InvocationContext
from ..starlark.invocation_context import RelativeLabel
from ..starlark.provider import TargetInfo
from ..starlark.select import Configurable
from ..util import quote_path
from .register import register_bzl_library
from .upb_proto_library import UPB_PLUGIN  # pylint: disable=unused-import

GRPC_REPO = RepositoryId("grpc")

_SEP = "\n        "


def _empty_target_list(t: TargetId) -> List[TargetId]:
  del t
  return []


_GRPC = PluginSettings(
    name="grpc",
    language="grpc",
    plugin=GRPC_REPO.parse_target("//src/compiler:grpc_cpp_plugin"),
    exts=[".grpc.pb.h", ".grpc.pb.cc"],
    runtime=[GRPC_REPO.parse_target("//:grpc++_codegen_proto")],
    aspectdeps=_empty_target_list,
)


@register_bzl_library("@grpc//bazel:generate_cc.bzl")
class GrpcGenerateCcLibrary(ScopeCommon):

  def bazel_generate_cc(
      self,
      well_known_protos: Any,
      name: str,
      visibility: Optional[List[RelativeLabel]] = None,
      **kwargs,
  ):
    context = self._context.snapshot()
    target = context.resolve_target(name)
    context.add_rule(
        target,
        lambda: _generate_grpc_cc_impl(
            context,
            target,
            well_known_protos=well_known_protos,
            **kwargs,
        ),
        analyze_by_default=False,
    )


def _generate_grpc_cc_impl(
    _context: InvocationContext,
    _target: TargetId,
    srcs: Optional[Configurable[List[RelativeLabel]]] = None,
    plugin: Optional[Configurable[RelativeLabel]] = None,
    flags: Optional[List[str]] = None,
    **kwargs,
):
  del kwargs
  state = _context.access(EvaluationState)
  repo = state.workspace.all_repositories.get(
      _context.caller_package_id.repository_id
  )
  assert repo is not None

  # Only a single source is allowed.
  resolved_srcs = _context.resolve_target_or_label_list(
      _context.evaluate_configurable_list(srcs)
  )
  assert len(resolved_srcs) == 1
  proto_target = resolved_srcs[0]

  plugin_settings = _GRPC
  if plugin is not None:
    resolved_plugin = _context.resolve_target_or_label(
        cast(RelativeLabel, _context.evaluate_configurable(plugin))
    )
    plugin_settings = PluginSettings(
        name=_GRPC.name,
        language=_GRPC.language,
        plugin=resolved_plugin,
        exts=_GRPC.exts,
        runtime=_GRPC.runtime,
        aspectdeps=_empty_target_list,
    )

  proto_target_info = _context.get_target_info(proto_target)
  proto_library_provider = proto_target_info.get(ProtoLibraryProvider)
  assert proto_library_provider is not None

  # Only generate code for proto files that contain a service.
  srcs_with_service = []
  for src in proto_library_provider.srcs:
    with open(_context.get_source_file_path(src), "r") as f:
      for line in f:
        if line.startswith("service "):
          srcs_with_service.append(src)
          break

  cmake_target_pair = state.generate_cmake_target_pair(_target, alias=False)

  # Construct the generated paths, installing this rule as a dependency.
  # TODO: Handle skip_import_prefix?
  proto_cmake_target = state.generate_cmake_target_pair(
      proto_library_provider.bazel_target
  ).target

  import_targets = set(
      state.collect_deps(proto_library_provider.deps).link_libraries()
  )
  import_targets.add(proto_cmake_target)

  # Emit the generated files.
  # See also native_aspect_proto.py
  generated_files = []
  for src in srcs_with_service:
    for t in plugin_generated_files(
        src,
        plugin_settings,
        repo.cmake_binary_dir,
    ):
      _context.add_analyzed_target(
          t[0],
          TargetInfo(
              CMakeAddDependenciesProvider(cmake_target_pair.target),
              FilesProvider([t[1]]),
          ),
      )
      generated_files.append(t[1])
  _context.add_analyzed_target(
      _target,
      TargetInfo(
          *make_providers(
              cmake_target_pair,
              CMakePackageDepsProvider,
              CMakeAddDependenciesProvider,
          ),
          FilesProvider(generated_files),
      ),
  )

  src_collector = state.collect_targets(srcs_with_service)
  state.collect_deps(UPB_PLUGIN.aspectdeps(srcs_with_service[0]))

  # Augment output with strip import prefix
  output_dir = repo.replace_with_cmake_macro_dirs([
      maybe_augment_output_dir(
          _context, proto_library_provider, repo.cmake_binary_dir
      )
  ])[0]

  # Emit.
  out = io.StringIO()
  out.write(f"\n# {_target.as_label()}\n")

  if not generated_files:
    out.write(f"add_custom_target({cmake_target_pair.target})\n")
    _context.access(CMakeBuilder).addtext(out.getvalue())
    return

  btc_protobuf(
      _context,
      out,
      plugin_settings=plugin_settings,
      # varname=str(cmake_target_pair.target),
      proto_src=next(src_collector.file_paths()),
      generated_files=generated_files,
      import_targets=import_targets,
      cmake_depends=set(src_collector.add_dependencies()),
      flags=flags,
      output_dir=repo.replace_with_cmake_macro_dirs([output_dir])[0],
  )
  out.write(f"add_custom_target({cmake_target_pair.target} DEPENDS")
  for x in generated_files:
    out.write(f"\n    {quote_path(x)}")
  out.write(")\n")

  _context.access(CMakeBuilder).addtext(out.getvalue())
