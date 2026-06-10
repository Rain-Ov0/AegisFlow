RULE login_fail_review
SCENE login
PRIORITY 100
IF user.login_fail_5m >= 5
THEN REVIEW REASON "too_many_failed_login"

RULE ip_attack_review
SCENE login
PRIORITY 90
IF user.login_fail_5m >= 3 AND ip.distinct_user_10m >= 20
THEN REVIEW REASON "ip_many_users_failed_login"

RULE topk_ip_review
SCENE all
PRIORITY 50
IF ip.in_topk == true AND cms.risk_behavior_count >= 100
THEN REVIEW REASON "hot_ip_high_frequency"