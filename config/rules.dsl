# Grammar:
# RULE <name>
# SCENE <scene|all>
# PRIORITY <int>
# IF <expr>
# THEN <PASS|REVIEW|REJECT> REASON "<reason_code>"
#
# Supported operators:
# number: == != > >= < <=
# bool:   == !=

RULE black_user_reject
SCENE all
PRIORITY 1000
IF user.black_hit == true
THEN REJECT REASON "blacklisted_user"

RULE black_ip_reject
SCENE all
PRIORITY 1000
IF ip.black_hit == true
THEN REJECT REASON "blacklisted_ip"

RULE black_device_reject
SCENE all
PRIORITY 1000
IF device.black_hit == true
THEN REJECT REASON "blacklisted_device"

RULE login_fail_review
SCENE login
PRIORITY 100
IF user.login_fail_5m >= 5
THEN REVIEW REASON "too_many_failed_login"

RULE login_burst_review
SCENE login
PRIORITY 95
IF user.login_1m >= 20 OR user.login_5m >= 60
THEN REVIEW REASON "login_frequency_burst"

RULE ip_attack_review
SCENE login
PRIORITY 90
IF user.login_fail_5m >= 3 AND ip.distinct_user_10m >= 20
THEN REVIEW REASON "ip_many_users_failed_login"

RULE device_many_accounts_review
SCENE login
PRIORITY 80
IF device.distinct_account_10m >= 10
THEN REVIEW REASON "device_many_accounts"

RULE severe_credential_stuffing_reject
SCENE login
PRIORITY 500
IF user.login_fail_5m >= 10 AND ip.distinct_user_10m >= 50
THEN REJECT REASON "credential_stuffing_attack"

RULE topk_ip_review
SCENE all
PRIORITY 50
IF ip.in_topk == true AND cms.risk_behavior_count >= 100
THEN REVIEW REASON "hot_ip_high_frequency"

RULE topk_ip_high_risk_reject
SCENE all
PRIORITY 300
IF ip.in_topk == true AND cms.risk_behavior_count >= 500
THEN REJECT REASON "hot_ip_extreme_frequency"