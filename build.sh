#!/bin/bash
# 构建脚本: 编译 + 签名 (+ 可选部署)
#
# 签名策略(重要): TCC 授权(输入监控/辅助功能)绑定在 **签名身份 + 二进制绝对路径** 上。
#  - 换签名身份 → 授权失效; 因此推荐创建一个长期自签证书, 名字固定为 tpkbd-sign:
#
#   钥匙串访问 → 证书助理 → 创建证书 → 名称 tpkbd-sign
#     类型: 代码签名  /  覆盖默认值勾上(有效期随意, e.g. 10 年)
#   创建后: 系统设置 → 隐私与安全性 → 输入监控/辅助功能 里授权本程序一次即可
#
#  - 移动/改名二进制 → 授权同样失效! 因此建议把守护进程部署到一个
#    不会再动的稳定路径, LaunchAgent 指向那里, 仓库只当源码用:
#
#   TPKB_INSTALL_DIR="$HOME/Library/Application Support/tpkbd-mac" ./build.sh
#
# 也可用环境变量指定别的身份: SIGN_ID="我的证书" ./build.sh
set -e
cd "$(dirname "$0")"

SIGN_ID="${SIGN_ID:-tpkbd-sign}"

clang -O2 -Wall -framework CoreFoundation -framework IOKit \
      -framework CoreGraphics -framework ApplicationServices -framework CoreVideo \
      -o tpkbd_touchpad tpkbd_touchpad.c

if security find-identity -v -p codesigning | grep -q "\"$SIGN_ID\""; then
    codesign -s "$SIGN_ID" -f -i com.pjr.tpkbd-touchpad tpkbd_touchpad
    echo "BUILD+SIGN OK ($SIGN_ID)"
else
    # 没有证书: ad-hoc 签名(能跑, 但每次重编译后 TCC 授权会失效, 需重新授权)
    codesign -s - -f -i com.pjr.tpkbd-touchpad tpkbd_touchpad
    echo "BUILD OK, ad-hoc signed (警告: 建议按脚本头部说明创建 tpkbd-sign 证书)"
fi

# 可选部署: 指定 TPKB_INSTALL_DIR 时, 把签名后的二进制复制到该稳定路径
# (LaunchAgent 的 ProgramArguments 应指向那里的副本; 重编译后重跑本脚本即可, 授权不失效)
if [ -n "$TPKB_INSTALL_DIR" ]; then
    mkdir -p "$TPKB_INSTALL_DIR"
    cp tpkbd_touchpad "$TPKB_INSTALL_DIR/tpkbd_touchpad"
    echo "DEPLOYED → $TPKB_INSTALL_DIR/tpkbd_touchpad"
    echo "(若守护进程在跑, 记得: launchctl kickstart -k gui/\$(id -u)/com.pjr.tpkbd-touchpad)"
fi
