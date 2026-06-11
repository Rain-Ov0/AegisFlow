CREATE DATABASE IF NOT EXISTS aegisflow
    DEFAULT CHARACTER SET utf8mb4
    DEFAULT COLLATE utf8mb4_unicode_ci;

USE aegisflow;

CREATE TABLE IF NOT EXISTS risk_blacklist (
    id BIGINT PRIMARY KEY AUTO_INCREMENT,
    entity_type VARCHAR(16) NOT NULL,
    entity_id VARCHAR(128) NOT NULL,
    reason VARCHAR(256) NOT NULL,
    enabled TINYINT NOT NULL DEFAULT 1,
    expire_at DATETIME NULL,
    created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    UNIQUE KEY uk_entity (entity_type, entity_id),
    INDEX idx_enabled_expire (enabled, expire_at)
);

INSERT INTO risk_blacklist(entity_type, entity_id, reason, enabled)
VALUES
('user', 'u_black_001', 'blacklisted_user', 1),
('ip', '10.0.0.9', 'blacklisted_ip', 1),
('device', 'dev_black_001', 'blacklisted_device', 1)
ON DUPLICATE KEY UPDATE
    reason = VALUES(reason),
    enabled = VALUES(enabled),
    updated_at = CURRENT_TIMESTAMP;