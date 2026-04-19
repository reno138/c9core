-- Fix transguid signedness (was INT, should be INT UNSIGNED consistent with all other GUID columns)
ALTER TABLE `characters` MODIFY COLUMN `transguid` INT UNSIGNED NOT NULL DEFAULT 0;

-- Track which cluster node owns each online character session.
-- Set to this node's ClusterServer.NodeId on login, cleared to 0 on logout.
-- Used for node-scoped crash recovery: on startup each node resets online=0 only
-- for its own node_id, leaving other nodes' sessions untouched.
-- Also cleared by the dead-node handler when a peer node is declared dead.
ALTER TABLE `characters`
    ADD COLUMN `owning_node_id` TINYINT UNSIGNED NOT NULL DEFAULT 0 AFTER `online`,
    ADD INDEX `idx_owning_node_id` (`owning_node_id`);
