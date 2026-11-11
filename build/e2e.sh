#!/usr/bin/env bash
# 临时端到端测试：走 HTTP 接口验证 发货 / 确认收货 / 退货全流程
set -u
BASE="http://127.0.0.1:8099"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
BUYER="$TMP/buyer.jar"; SELLER="$TMP/seller.jar"; ADMIN="$TMP/admin.jar"; DEV="$TMP/dev.jar"

fail=0
check() { # check <desc> <json> <needle>
  if echo "$2" | grep -q "$3"; then echo "ok   $1"; else echo "FAIL $1 -> $2"; fail=1; fi
}

post() { # post <jar> <path> <body>
  curl -s -b "$1" -c "$1" -H 'Content-Type: application/json' -d "$3" "$BASE$2"
}
login() { post "$1" /api/login "{\"username\":\"$2\",\"password\":\"$3\"}" > /dev/null; }

# --- 账号 ---
post "$DEV" /api/dev/account '{"username":"e2b","password":"passw0rd","role":1}' > /dev/null
post "$DEV" /api/dev/account '{"username":"e2s","password":"passw0rd","role":2}' > /dev/null
post "$DEV" /api/dev/account '{"username":"e2a","password":"passw0rd","role":4}' > /dev/null
login "$BUYER" e2b passw0rd
login "$SELLER" e2s passw0rd
login "$ADMIN" e2a passw0rd
check "买家登录" "$(curl -s -b "$BUYER" $BASE/api/me)" '"username":"e2b"'

# --- 卖家建商品并发布 ---
PID=$(post "$SELLER" /api/products '{}' | sed 's/.*"productId":\([0-9]*\).*/\1/')
post "$SELLER" /api/products/draft "{\"productId\":$PID,\"title\":\"e2e\",\"description\":\"d\",\"category\":\"c\",\"price\":100,\"remaining\":3}" > /dev/null
RID=$(post "$SELLER" /api/products/submit "{\"productId\":$PID}" | sed 's/.*"reviewApplicationId":\([0-9]*\).*/\1/')
check "管理员审核通过" "$(post "$ADMIN" /api/review "{\"reviewApplicationId\":$RID,\"approval\":true,\"reason\":\"\"}")" '"ok":true'

# --- 买家充值、预约；卖家确认；买家付款 ---
post "$BUYER" /api/account/recharge '{"amount":1000}' > /dev/null
RES=$(post "$BUYER" /api/reservations "{\"productId\":$PID,\"amount\":1}" | sed 's/.*"reservationId":\([0-9]*\).*/\1/')
post "$SELLER" /api/reservations/confirm "{\"reservationId\":$RES,\"changedPrice\":-1}" > /dev/null
ORDER=$(post "$BUYER" /api/reservations/buy "{\"reservationId\":$RES}" | sed 's/.*"orderId":\([0-9]*\).*/\1/')
check "付款生成订单" "$(curl -s -b "$BUYER" $BASE/api/escrow)" '"balance":100'

# --- 越权：买家不能发货 ---
check "买家发货被拒" "$(post "$BUYER" /api/orders/ship "{\"orderId\":$ORDER}")" '"code":3'
# --- 卖家发货，买家确认收货 ---
check "卖家发货" "$(post "$SELLER" /api/orders/ship "{\"orderId\":$ORDER}")" '"ok":true'
check "买家重复收货前状态错误" "$(post "$BUYER" /api/orders/receipt "{\"orderId\":999}")" '"code":4'
check "买家确认收货" "$(post "$BUYER" /api/orders/receipt "{\"orderId\":$ORDER}")" '"ok":true'
check "托管已清零" "$(curl -s -b "$BUYER" $BASE/api/escrow)" '"balance":0'
check "订单变为 Completed" "$(curl -s -b "$BUYER" $BASE/api/orders)" '"status":3'

# --- 第二条订单走退货流程 ---
RES2=$(post "$BUYER" /api/reservations "{\"productId\":$PID,\"amount\":1}" | sed 's/.*"reservationId":\([0-9]*\).*/\1/')
post "$SELLER" /api/reservations/confirm "{\"reservationId\":$RES2,\"changedPrice\":-1}" > /dev/null
ORDER2=$(post "$BUYER" /api/reservations/buy "{\"reservationId\":$RES2}" | sed 's/.*"orderId":\([0-9]*\).*/\1/')
post "$SELLER" /api/orders/ship "{\"orderId\":$ORDER2}" > /dev/null

check "买家申请退货" "$(post "$BUYER" /api/returns "{\"orderId\":$ORDER2,\"reason\":\"坏了\"}")" '"ok":true'
RET=$(curl -s -b "$BUYER" $BASE/api/returns | sed 's/.*"id":\([0-9]*\),"orderId".*/\1/')
check "非管理员审核被拒" "$(post "$BUYER" /api/returns/review "{\"returnId\":$RET,\"approval\":true}")" '"code":3'
check "驳回无理由被拒" "$(post "$ADMIN" /api/returns/review "{\"returnId\":$RET,\"approval\":false,\"reason\":\"\"}")" '"code":1'
check "管理员通过退货" "$(post "$ADMIN" /api/returns/review "{\"returnId\":$RET,\"approval\":true,\"reason\":\"同意\"}")" '"ok":true'
check "买家填快递单号" "$(post "$BUYER" /api/returns/delivered "{\"returnId\":$RET,\"trackingNo\":\"SF1\"}")" '"ok":true'
check "卖家确认收到" "$(post "$SELLER" /api/returns/received "{\"returnId\":$RET}")" '"ok":true'
check "退款后托管归零" "$(curl -s -b "$BUYER" $BASE/api/escrow)" '"balance":0'
check "订单变为 Returned" "$(curl -s -b "$BUYER" $BASE/api/orders)" '"status":5'
check "退货状态 Refunded" "$(curl -s -b "$BUYER" $BASE/api/returns)" '"status":5'
check "买家余额 900（第二单已退款）" "$(curl -s -b "$BUYER" $BASE/api/me)" '"balance":900'
check "卖家余额为 100（第一单）" "$(curl -s -b "$SELLER" $BASE/api/me)" '"balance":100'

exit $fail
