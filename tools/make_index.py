#!/usr/bin/env python3
# 生成 dist/index.json —— ltool 的插件商店读的就是这一份。
#
# 为什么现算而不是手维护一份清单：每 dll 的 SHA256 和字节数**只能从产物来**，
# 手抄的那份第一次重编就过期；商店装之前要按这个哈希校验，哈希错了就是"下载到的不是编出来的那个"。
# 名字/描述/关键词/API 也一样：从各插件目录里的 plugin.json 读，不在这里再写一遍。
#
# 用法：
#   python tools/make_index.py --build build/plugins --src plugins --out dist/index.json \
#       --base-url https://github.com/mateo-ki/ltool_plugin/releases/download/<tag>
import argparse
import hashlib
import json
import os
import sys


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for blk in iter(lambda: f.read(1 << 20), b''):
            h.update(blk)
    return h.hexdigest()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--build', required=True, help='编好的 dll 所在目录')
    ap.add_argument('--src', required=True, help='plugins/ 目录（每个子目录一份 plugin.json）')
    ap.add_argument('--out', required=True)
    ap.add_argument('--base-url', default='', help='产物下载前缀；留空就只给文件名')
    ap.add_argument('--sdk-info', default='', help='sdk/INFO.txt，把 core 版本一起写进去')
    a = ap.parse_args()

    core = ''
    if a.sdk_info and os.path.exists(a.sdk_info):
        for line in open(a.sdk_info, encoding='utf-8'):
            if line.startswith('来自 ltool '):
                core = line.strip().rstrip('（')
                break
    out = []
    for d in sorted(os.listdir(a.src)):
        pj = os.path.join(a.src, d, 'plugin.json')
        if not os.path.exists(pj):
            continue
        meta = json.load(open(pj, encoding='utf-8'))
        dll = os.path.join(a.build, 'ltool-%s.dll' % meta['Id'])
        if not os.path.exists(dll):
            print('FAIL 插件 %s 有 plugin.json 但产物里没有 %s —— 构建没成功就别进索引'
                  % (d, os.path.basename(dll)))
            sys.exit(1)
        name = 'ltool-%s.dll' % meta['Id']
        row = {'id': meta['Id'], 'name': meta['Name'], 'desc': meta['Desc'],
               'api': meta['Api'], 'keywords': meta.get('Keywords', []),
               'actions': meta.get('Actions', []), 'color': meta.get('Color', ''),
               'file': name, 'bytes': os.path.getsize(dll), 'sha256': sha256(dll)}
        row['url'] = (a.base_url.rstrip('/') + '/' + name) if a.base_url else name
        out.append(row)
    doc = {'schema': 1, 'core': core, 'plugins': out}
    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    with open(a.out, 'w', encoding='utf-8') as f:
        json.dump(doc, f, ensure_ascii=False, indent=1)
    print('index.json：%d 个插件 -> %s' % (len(out), a.out))
    for r in out:
        print('  %-9s api=%s %7d B  %s' % (r['id'], r['api'], r['bytes'], r['sha256'][:12]))


main()
