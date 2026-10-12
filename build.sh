#!/bin/bash
# 构建脚本: 编译 + 签名
#
# 签名策略(重要): TCC 授权(输入监控/辅助功能)绑定在签名身份上。
# 若每次重编译换身份, 授权就失效、要反复去系统设置里重新允许。
# 因此推荐创建一个长期自签证书, 名字固定为 tpkbd-sign:
#
#   钥匙串访问 → 证书助理 → 创建证书 → 名称 tpkbd-sign
#     类型: 代码签名  /  覆盖默认值勾上(有效期随意, e.g. 10 年)
#   创建后: 系统设置 → 隐私与安全性 → 输入监控/辅助功能 里授权本程序一次即可
#
# 也可用环境变量指定别的身份: SIGN_ID="我的证书" ./build.sh
set -e
cd "$(dirname "$0")"

SIGN_ID="${SIGN_ID:-tpkbd-sign}"

clang -O2 -Wall -framework CoreFoundation -framework IOKit \
      -framework CoreGraphics -framework ApplicationServices \
      -o tpkbd_touchpad tpkbd_touchpad.c

if security find-identity -v -p codesigning | grep -q "\"$SIGN_ID\""; then
    codesign -s "$SIGN_ID" -f -i com.pjr.tpkbd-touchpad tpkbd_touchpad
    echo "BUILD+SIGN OK ($SIGN_ID)"
else
    # 没有证书: ad-hoc 签名(能跑, 但每次重编译后 TCC 授权会失效, 需重新授权)
    codesign -s - -f -i com.pjr.tpkbd-touchpad tpkbd_touchpad
    echo "BUILD OK, ad-hoc signed (警告: 建议按脚本头部说明创建 tpkbd-sign 证书)"
fi
