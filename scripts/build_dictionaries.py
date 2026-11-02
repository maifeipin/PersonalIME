#!/usr/bin/env python3
"""
Convert Rime raw YAML dictionaries into clean TSV format for PersonalIME.
"""
import os
import re
import urllib.request

WUBI_URL = "https://raw.githubusercontent.com/rime/rime-wubi/master/wubi86.dict.yaml"
PINYIN_URL = "https://raw.githubusercontent.com/rime/rime-pinyin-simp/master/pinyin_simp.dict.yaml"

def ensure_file(path, url):
    if not os.path.exists(path):
        os.makedirs(os.path.dirname(path), exist_ok=True)
        print(f"Downloading {url} to {path}...")
        urllib.request.urlretrieve(url, path)

def process_wubi():
    in_path = os.path.join("data", "dictionaries", "wubi86.raw.yaml")
    ensure_file(in_path, WUBI_URL)
    out_path = os.path.join("data", "dictionaries", "wubi86.dict")
    count = 0
    with open(in_path, "r", encoding="utf-8") as f_in, open(out_path, "w", encoding="utf-8") as f_out:
        past_header = False
        for line in f_in:
            line = line.strip()
            if not past_header:
                if line == "...":
                    past_header = True
                continue
            if not line or line.startswith("#"):
                continue
            parts = line.split("\t")
            if len(parts) >= 2:
                text = parts[0].strip()
                code = parts[1].strip().lower()
                weight = 0
                if len(parts) >= 3 and parts[2].strip().isdigit():
                    weight = int(parts[2].strip())
                # Normalize weight to 0-1000 range for reasonable base score
                norm_weight = min(1000, max(1, int(weight / 1000000))) if weight > 0 else 10
                f_out.write(f"{text}\t{code}\t{norm_weight}\n")
                count += 1
    print(f"Processed Wubi86: {count} entries written to {out_path}")
    return count

def process_pinyin():
    in_path = os.path.join("data", "dictionaries", "pinyin_simp.raw.yaml")
    ensure_file(in_path, PINYIN_URL)
    out_path = os.path.join("data", "dictionaries", "pinyin.dict")
    count = 0
    with open(in_path, "r", encoding="utf-8") as f_in, open(out_path, "w", encoding="utf-8") as f_out:
        past_header = False
        for line in f_in:
            line = line.strip()
            if not past_header:
                if line == "...":
                    past_header = True
                continue
            if not line or line.startswith("#"):
                continue
            parts = line.split("\t")
            if len(parts) >= 2:
                text = parts[0].strip()
                syllables_str = parts[1].strip().lower()
                # Continuous code is syllables concatenated without spaces
                continuous_code = syllables_str.replace(" ", "")
                weight = 10
                if len(parts) >= 3 and parts[2].strip().isdigit():
                    weight = int(parts[2].strip())
                norm_weight = min(1000, max(1, weight))
                f_out.write(f"{text}\t{continuous_code}\t{syllables_str}\t{norm_weight}\n")
                count += 1
    print(f"Processed Pinyin: {count} entries written to {out_path}")
    return count

if __name__ == "__main__":
    wubi_count = process_wubi()
    pinyin_count = process_pinyin()
    print(f"Total entries: {wubi_count + pinyin_count}")

