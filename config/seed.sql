USE aegisflow;

INSERT INTO risk_blacklist_user(user_id, reason, enabled, expire_at)
VALUES ('u_black_001', 'blacklisted_user', TRUE, NULL)
ON DUPLICATE KEY UPDATE
    reason = 'blacklisted_user', enabled = TRUE, expire_at = NULL;

INSERT INTO risk_blacklist_ip(ip, reason, enabled, expire_at)
VALUES (INET6_ATON('10.0.0.9'), 'blacklisted_ip', TRUE, NULL)
ON DUPLICATE KEY UPDATE
    reason = 'blacklisted_ip', enabled = TRUE, expire_at = NULL;

INSERT INTO risk_blacklist_device(device_id, reason, enabled, expire_at)
VALUES ('dev_black_001', 'blacklisted_device', TRUE, NULL)
ON DUPLICATE KEY UPDATE
    reason = 'blacklisted_device', enabled = TRUE, expire_at = NULL;
