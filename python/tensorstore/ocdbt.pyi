from __future__ import annotations
import tensorstore
import typing

__all__ = ["DistributedCoordinatorServer", "dump"]


class DistributedCoordinatorServer:
    """

    Distributed coordinator server for the OCDBT (Optionally-Cooperative Distributed
    B+Tree) database.

    Example:

        >> server = ts.ocdbt.DistributedCoordinatorServer()

    Group:
      OCDBT

    """

    def __init__(self, json: typing.Any = {}) -> None: ...

    @property
    def port(self) -> int: ...


def dump(
    base: tensorstore.KvStore,
    node: str | None = None,
    *,
    context: tensorstore.Context | None = None,
) -> tensorstore.Future[typing.Any]:
    """
    Dumps the internal representation of an OCDBT database.

    Args:
      base: Base kvstore containing the OCDBT database.

      node: Reference to the node or value to dump, of the form
        ``"<type>:<file-id>:<offset>:<length>"`` where ``<type>`` is one of
        ``"value"``, ``"btreenode"``, or ``"versionnode"``, as specified in a
        ``"location"`` field within the manifest, a B+tree node, or a version node.
        If not specified, the manifest is dumped.

      context: Context from which the :json:schema:`Context.cache_pool` and
        :json:schema:`Context.data_copy_concurrency` resources will be used.  If not
        specified, a new default context is used.

    Returns:
      The manifest or node representation as JSON (augmented to include byte
      strings), or the value as a byte string.

    Group:
      OCDBT

    Examples:
    ---------

      >>> store = ts.KvStore.open({
      ...     "driver": "ocdbt",
      ...     "config": {
      ...         "max_inline_value_bytes": 1
      ...     },
      ...     "base": "memory://"
      ... }).result()
      >>> store["a"] = b"b"
      >>> store["b"] = b"ce"
      >>> manifest = ts.ocdbt.dump(store.base).result()
      >>> manifest
      {'config': {'compression': {'id': 'zstd'},
                  'max_decoded_node_bytes': 8388608,
                  'max_inline_value_bytes': 1,
                  'uuid': '...',
                  'version_tree_arity_log2': 4},
       'version_tree_nodes': [],
       'versions': [{'commit_time': ...,
                     'generation_number': 1,
                     'root': {'statistics': {'num_indirect_value_bytes': 0,
                                             'num_keys': 0,
                                             'num_tree_bytes': 0}},
                     'root_height': 0},
                    {'commit_time': ...,
                     'generation_number': 2,
                     'root': {'location': 'btreenode::d/...:0:35',
                              'statistics': {'num_indirect_value_bytes': 0,
                                             'num_keys': 1,
                                             'num_tree_bytes': 35}},
                     'root_height': 0},
                    {'commit_time': ...,
                     'generation_number': 3,
                     'root': {'location': 'btreenode::d/...:2:78',
                              'statistics': {'num_indirect_value_bytes': 2,
                                             'num_keys': 2,
                                             'num_tree_bytes': 78}},
                     'root_height': 0}]}
      >>> btree = ts.ocdbt.dump(
      ...     store.base, manifest["versions"][-1]["root"]["location"]).result()
      >>> btree
      {'entries': [{'inline_value': b'b', 'key': b'a'},
                   {'indirect_value': 'value::d/...:0:2',
                    'key': b'b'}],
       'height': 0}
      >>> ts.ocdbt.dump(store.base,
      ...               btree["entries"][1]["indirect_value"]).result()
      b'ce'
    """
