import random
import string
import threading

import pytest

from helpers.cluster import ClickHouseCluster

cluster = ClickHouseCluster(__file__)

NUM_WORKERS = 5
MAX_ROWS = 1000


def gen_insert_values(size):
    return ",".join(
        f"({i},'{''.join(random.choices(string.ascii_lowercase, k=5))}')"
        for i in range(size)
    )


insert_values = ",".join(
    f"({i},'{''.join(random.choices(string.ascii_lowercase, k=5))}')" for i in range(10)
)


@pytest.fixture(scope="module", autouse=True)
def start_cluster():
    for i in range(NUM_WORKERS):
        cluster.add_instance(
            f"node{i + 1}",
            main_configs=["configs/storage_conf.xml"],
            with_minio=True,
            env_variables={"ENDPOINT_SUBPATH": f"node{i + 1}"},
            stay_alive=True,
            # Override ENDPOINT_SUBPATH.
            instance_env_variables=i > 0,
        )

    try:
        cluster.start()
        yield cluster
    finally:
        cluster.shutdown()


@pytest.mark.parametrize(
    "storage_policy,key_prefix",
    [
        pytest.param("s3_plain_rewritable", "data/"),
        pytest.param("cache_s3_plain_rewritable", "data/"),
        pytest.param("s3_plain_rewritable_with_metadata_cache", "data_with_cache/"),
    ],
)
def test(storage_policy, key_prefix):
    def create_insert(node, table_name, insert_values):
        node.query(
            """
            CREATE TABLE {} (
                id Int64,
                data String
            ) ENGINE=MergeTree()
            PARTITION BY id % 10
            ORDER BY id
            SETTINGS storage_policy='{}'
            """.format(
                table_name, storage_policy
            )
        )
        if insert_values:
            node.query("INSERT INTO {} VALUES {}".format(table_name, insert_values))

    insert_values_arr = [
        gen_insert_values(random.randint(1, MAX_ROWS)) for _ in range(0, NUM_WORKERS)
    ]
    threads = []
    assert len(cluster.instances) == NUM_WORKERS
    for i in range(NUM_WORKERS):
        node = cluster.instances[f"node{i + 1}"]
        for table_name, values in [("test", insert_values_arr[i]), ("test_dst", "")]:
            t = threading.Thread(target=create_insert, args=(node, table_name, values))
            threads.append(t)
            t.start()

    for t in threads:
        t.join()

    for i in range(NUM_WORKERS):
        node = cluster.instances[f"node{i + 1}"]
        assert (
            node.query("SELECT * FROM test ORDER BY id FORMAT Values")
            == insert_values_arr[i]
        )

    for i in range(NUM_WORKERS):
        node = cluster.instances[f"node{i + 1}"]
        node.query("ALTER TABLE test MODIFY SETTING old_parts_lifetime = 59")
        assert (
            node.query(
                "SELECT engine_full from system.tables WHERE database = currentDatabase() AND name = 'test'"
            ).find("old_parts_lifetime = 59")
            != -1
        )

        node.query("ALTER TABLE test RESET SETTING old_parts_lifetime")
        assert (
            node.query(
                "SELECT engine_full from system.tables WHERE database = currentDatabase() AND name = 'test'"
            ).find("old_parts_lifetime")
            == -1
        )
        node.query("ALTER TABLE test MODIFY COMMENT 'new description'")
        assert (
            node.query(
                "SELECT comment from system.tables WHERE database = currentDatabase() AND name = 'test'"
            ).find("new description")
            != -1
        )

        count_part_0 = int(node.query("SELECT count(*) FROM test WHERE id % 10 = 0"))
        node.query("ALTER TABLE test MOVE PARTITION 0 TO TABLE test_dst")

        count_part_1 = int(node.query("SELECT count(*) FROM test WHERE id % 10 = 1"))
        node.query("ALTER TABLE test_dst REPLACE PARTITION 1 FROM test")

        count_dst = int(node.query("SELECT count(*) FROM test_dst"))
        assert count_dst > 0
        assert count_dst == count_part_0 + count_part_1

    insert_values_arr = []
    for i in range(NUM_WORKERS):
        node = cluster.instances[f"node{i + 1}"]
        insert_values_arr.append(
            node.query("SELECT * FROM test ORDER BY id FORMAT Values")
        )

    def restart(node):
        node.restart_clickhouse()

    threads = []
    for i in range(NUM_WORKERS):
        node = cluster.instances[f"node{i + 1}"]
        t = threading.Thread(target=restart, args=(node,))
        threads.append(t)
        t.start()

    for t in threads:
        t.join()

    for i in range(NUM_WORKERS):
        node = cluster.instances[f"node{i + 1}"]
        assert (
            node.query("SELECT * FROM test ORDER BY id FORMAT Values")
            == insert_values_arr[i]
        )

    metadata_it = cluster.minio_client.list_objects(
        cluster.minio_bucket, key_prefix, recursive=True
    )
    metadata_count = 0
    for obj in list(metadata_it):
        if "/__meta/" in obj.object_name:
            assert obj.object_name.endswith("/prefix.path")
            metadata_count += 1
        else:
            assert not obj.object_name.endswith("/prefix.path")

    assert metadata_count > 0

    for i in range(NUM_WORKERS):
        node = cluster.instances[f"node{i + 1}"]
        node.query("DROP TABLE IF EXISTS test SYNC")
        node.query("DROP TABLE IF EXISTS test_dst SYNC")

    it = cluster.minio_client.list_objects(
        cluster.minio_bucket, key_prefix, recursive=True
    )

    assert len(list(it)) == 0
