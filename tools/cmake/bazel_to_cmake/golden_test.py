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


# pylint: disable=relative-beyond-top-level,wildcard-import

import json
import os
import pathlib
import shutil
import sys
from typing import Any, Dict, List, Tuple, Union

import pytest

from . import native_rules  # pylint: disable=unused-import
from .active_repository import Repository
from .cmake_repository import CMakeRepository
from .cmake_repository import make_repo_mapping
from .cmake_target import CMakePackage
from .cmake_target import CMakeTarget
from .cmake_target import CMakeTargetPair
from .evaluation import EvaluationState
from .platforms import add_platform_constraints
from .starlark import rule  # pylint: disable=unused-import
from .starlark.bazel_target import RepositoryId
from .starlark.bazel_target import TargetId
from .util import is_relative_to
from .workspace import Workspace

# NOTE: Consider adding failure tests as well as the success tests.

# To update, run:
#   UPDATE_GOLDENS=1 python3 -m pytest bazel_to_cmake/golden_test.py
#
UPDATE_GOLDENS = os.getenv('UPDATE_GOLDENS') == '1'

CMAKE_VARS = {
    'CMAKE_CXX_COMPILER_ID': 'Clang',
    'CMAKE_SYSTEM_NAME': 'Linux',
    'CMAKE_SYSTEM_PROCESSOR': 'AMD64',
    'CMAKE_COMMAND': 'cmake',
    'PROJECT_IS_TOP_LEVEL': 'YES',
    'CMAKE_FIND_PACKAGE_REDIRECTS_DIR': '_find_pkg_redirects_',
    'CMAKE_MESSAGE_LOG_LEVEL': 'TRACE',
}


def parameters() -> List[Tuple[str, Dict[str, Any]]]:
  """Returns config tuples by reading config.json from the 'testdata' subdir."""
  if UPDATE_GOLDENS:
    print('UPDATE_GOLDENS')
    testdata = pathlib.Path(__file__).resolve().with_name('testdata')
  else:
    testdata = pathlib.Path(__file__).with_name('testdata').resolve()
  result: List[Tuple[str, Dict[str, Any]]] = []
  for x in testdata.iterdir():
    if '__' in str(x):
      continue
    try:
      with (x / 'config.json').open('r') as f:
        config: Dict[str, Any] = json.load(f)
    except FileNotFoundError as e:
      raise FileNotFoundError(f'Failed to read {str(x)}/config.json') from e
    config['source_directory'] = str(x)
    result.append((x.name, config))
  return result


def get_files_list(
    source_directory: pathlib.Path, include_goldens=False
) -> List[pathlib.Path]:
  """Returns non-golden files under source directory."""
  files: List[pathlib.Path] = []
  try:
    for x in sorted(source_directory.glob('**/*')):
      if not x.is_file():
        continue
      if 'golden/' in str(x) and not include_goldens:
        continue
      files.append(x)
  except FileNotFoundError as e:
    print(f'Failure to read {source_directory.as_posix()}: {e}')
  return files


def copy_tree(source_dir: str, source_files: List[str], dest_dir: pathlib.Path):
  """Copies source_files from source_dir to dest_dir."""
  for x in source_files:
    dest_path = dest_dir.joinpath(x)
    os.makedirs(os.path.dirname(dest_path), exist_ok=True)
    shutil.copy(os.path.join(source_dir, x), dest_path)


def compare_files(golden, generated):
  with pathlib.Path(golden).open('r') as right:
    with pathlib.Path(generated).open('r') as left:
      # TODO(jbms): disable comparison until SCRIPT_DIRECTORY/TEST_BINDIR
      # relative sort order can be resolved.
      pass
      # assert list(left) == list(right)


def add_repositories(workspace: Workspace):
  workspace.add_cmake_repository(
      CMakeRepository(
          RepositoryId('com_google_protobuf'),
          CMakePackage('Protobuf'),
          pathlib.PurePosixPath('protobuf_src'),
          pathlib.PurePosixPath('protobuf_build'),
          repo_mapping={},
          persisted_canonical_name={},
      )
  )
  workspace.add_cmake_repository(
      CMakeRepository(
          RepositoryId('grpc'),
          CMakePackage('gRPC'),
          pathlib.PurePosixPath('grpc_src'),
          pathlib.PurePosixPath('grpc_build'),
          repo_mapping={
              RepositoryId('com_google_protobuf'): RepositoryId(
                  'com_google_protobuf'
              )
          },
          persisted_canonical_name={},
      )
  )

  def persist_cmake_name(
      target: Union[str, TargetId],
      cmake_alias: CMakeTarget,
  ):
    if not isinstance(target, TargetId):
      target = workspace.root_repository.repository_id.parse_target(str(target))
    assert isinstance(target, TargetId)

    assert (
        target.repository_id in workspace.all_repositories
    ), target.repository_id
    repo = workspace.all_repositories[target.repository_id]

    cmake_target_pair: CMakeTargetPair = repo.get_cmake_target_pair(
        target
    ).with_alias(cmake_alias)
    repo.set_persisted_canonical_name(target, cmake_target_pair)

  # Add default mappings used in proto code.
  persist_cmake_name(
      '@com_google_protobuf//:protoc',
      CMakeTarget('protobuf::protoc'),
  )

  persist_cmake_name(
      '@com_google_protobuf//:protobuf',
      CMakeTarget('protobuf::libprotobuf'),
  )

  persist_cmake_name(
      '@com_google_protobuf//:protobuf_lite',
      CMakeTarget('protobuf::libprotobuf_lite'),
  )

  persist_cmake_name(
      '@com_google_protobuf//:any_protoc',
      CMakeTarget('protobuf::any_proto'),
  )

  # gRPC
  persist_cmake_name(
      '@grpc//:grpc++_codegen_proto',
      CMakeTarget('gRPC::gRPC_codegen'),
  )

  persist_cmake_name(
      '@grpc//src/compiler:grpc_cpp_plugin',
      CMakeTarget('gRPC::grpc_cpp_plugin'),
  )

  # upb
  persist_cmake_name(
      '@com_google_protobuf//upb_generator:protoc-gen-upb_minitable_stage1',
      CMakeTarget('protobuf::protoc_gen_upb_minitable_stage1'),
  )
  persist_cmake_name(
      '@com_google_protobuf//upb_generator:protoc-gen-upb',
      CMakeTarget('protobuf::protoc_gen_upb'),
  )
  persist_cmake_name(
      '@com_google_protobuf//upb_generator:protoc-gen-upb_stage1',
      CMakeTarget('protobuf::protoc_gen_upb_stage1'),
  )
  persist_cmake_name(
      '@com_google_protobuf//upb_generator:protoc-gen-upbdefs',
      CMakeTarget('protobuf::protoc_gen_upbdefs'),
  )

  persist_cmake_name(
      '@com_google_protobuf//upb:generated_code_support__only_for_generated_code_do_not_use__i_give_permission_to_break_me',
      CMakeTarget(
          'protobuf::upb_generated_code_support__only_for_generated_code_do_not_use__i_give_permission_to_break_me'
      ),
  )
  persist_cmake_name(
      '@com_google_protobuf//upb::generated_reflection_support__only_for_generated_code_do_not_use__i_give_permission_to_break_me',
      CMakeTarget(
          'protobuf::upb_generated_reflection_support__only_for_generated_code_do_not_use__i_give_permission_to_break_me'
      ),
  )
  persist_cmake_name(
      '@com_google_protobuf//upb::mini_table',
      CMakeTarget('protobuf::upb_mini_table'),
  )
  persist_cmake_name(
      '@com_google_protobuf//upb::port',
      CMakeTarget('protobuf::upb_port'),
  )


@pytest.mark.parametrize('test_name,config', parameters())
def test_golden(test_name: str, config: Dict[str, Any], tmpdir):
  # Start with the list of source files.
  source_directory = pathlib.Path(config['source_directory'])
  del config['source_directory']
  input_files = [
      str(x.relative_to(source_directory))
      for x in get_files_list(source_directory, include_goldens=False)
  ]

  # Create the working directory as a snapshot of the source directory.
  directory = pathlib.Path(tmpdir)
  srcdir = directory.joinpath('_srcdir')
  srcdir.mkdir(exist_ok=True, parents=True)
  copy_tree(str(source_directory), input_files, srcdir)

  bindir = directory.joinpath('_bindir')
  bindir.mkdir(exist_ok=True, parents=True)

  golden_directory = source_directory.joinpath('golden')

  os.chdir(srcdir)
  os.makedirs(CMAKE_VARS['CMAKE_FIND_PACKAGE_REDIRECTS_DIR'], exist_ok=True)

  repository_id = RepositoryId(f'{test_name}_test_repo')
  root_repository = CMakeRepository(
      repository_id=repository_id,
      cmake_project_name=CMakePackage('CMakeProject'),
      source_directory=srcdir,
      cmake_binary_dir=bindir,
      repo_mapping=make_repo_mapping(
          repository_id, config.get('repo_mapping', [])
      ),
      persisted_canonical_name={},
  )

  # Setup repo mapping.
  for x in config.get('repo_mapping', []):
    root_repository.repo_mapping[RepositoryId(x[0])] = RepositoryId(x[1])

  # Workspace setup
  workspace = Workspace(repository_id, CMAKE_VARS)
  workspace.save_workspace = '_workspace.pickle'
  workspace.host_platform_name = 'linux'
  workspace._verbose = 3
  workspace.add_cmake_repository(root_repository)

  add_platform_constraints(workspace)
  add_repositories(workspace)

  # Load specified modules.
  for x in config.get('modules', []):
    workspace.add_module(x)
  workspace.load_modules()

  # load bazelrc
  bazelrc_path = os.path.join(directory, '.bazelrc')
  if pathlib.Path(bazelrc_path).exists():
    workspace.load_bazelrc(bazelrc_path)

  # Setup active repository
  active_repo = Repository(
      workspace=workspace,
      repository_id=repository_id,
      bindings={},
      top_level=True,
  )

  # Evaluate the WORKSPACE and BUILD files
  state = EvaluationState(active_repo)
  state.process_workspace()

  for build_file in config.get('build_files', ['BUILD.bazel']):
    state.process_build_file(
        root_repository.source_directory.joinpath(build_file)
    )

  # Analyze
  if config.get('targets') is not None:
    targets_to_analyze = [
        active_repo.repository_id.parse_target(t) for t in config.get('targets')
    ]
  else:
    targets_to_analyze = sorted(state.targets_to_analyze)

  state.analyze(targets_to_analyze)

  # Write generated file
  pathlib.Path('build_rules.cmake').write_text(state.builder.as_text())

  # Collect the output files, excluding the input files,
  # and normalize the contents.
  excludes = config.get('excludes', [])
  files = []
  for p in get_files_list(directory):
    if is_relative_to(p, srcdir):
      r = str(p.relative_to(srcdir))
      if r in input_files:
        continue
    if str(p.relative_to(directory)) in excludes:
      continue

    txt = p.read_text()
    newtxt = txt
    for args in [
        (os.path.abspath(sys.argv[0]), 'run_bazel_to_cmake.py'),
        (str(bindir), '${TEST_BINDIR}'),
        (str(srcdir), '${TEST_SRCDIR}'),
        (os.path.dirname(__file__), '${SCRIPT_DIRECTORY}'),
    ]:
      newtxt = newtxt.replace(*args)
    if newtxt != txt:
      print(f'Rewriting {p}')
      p.write_text(newtxt)
    files.append(p)

  print(f'Found {files}')
  if UPDATE_GOLDENS:
    print(f'Updating goldens for {test_name} in {golden_directory}')
    try:
      shutil.rmtree(golden_directory)
    except FileNotFoundError:
      pass
    for p in files:
      x = str(p.relative_to(directory))
      dest_path = golden_directory.joinpath(x)
      dest_path.parent.mkdir(parents=True, exist_ok=True)
      shutil.copyfile(p, dest_path)

  # Assert files exist.
  golden_files = [
      x.relative_to(golden_directory)
      for x in get_files_list(golden_directory, include_goldens=True)
  ]
  assert len(golden_files) > 0  # pylint: disable=g-explicit-length-test
  for x in golden_files:
    # Assert on file contents.
    expected_file = os.path.join(golden_directory, str(x))
    actual_file = os.path.join(directory, str(x))
    compare_files(expected_file, actual_file)
