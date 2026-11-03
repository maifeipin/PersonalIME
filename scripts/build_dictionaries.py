#!/usr/bin/env python3
"""
Convert Rime raw YAML dictionaries into clean TSV format for PersonalIME.
Pinned upstream commit versions with cryptographic hash verification.
"""
import os
import re
import hashlib
import urllib.request

CONVERTER_VERSION = "2.0.0"

# Pinned immutable upstream commit SHAs
WUBI_COMMIT = "152a0d3f3efe40cae216d1e3b338242446848d07"
PINYIN_COMMIT = "0c6861ef7420ee780270ca6d993d18d4101049d0"

WUBI_URL = f"https://raw.githubusercontent.com/rime/rime-wubi/{WUBI_COMMIT}/wubi86.dict.yaml"
PINYIN_URL = f"https://raw.githubusercontent.com/rime/rime-pinyin-simp/{PINYIN_COMMIT}/pinyin_simp.dict.yaml"

EXPECTED_WUBI_RAW_SHA256 = "f833d86b72341fe82e069a425b6625f29ef85f1bc0f34f6fb7975fe514888b5a"
EXPECTED_WUBI_DICT_SHA256 = "a420adda6a5e36deb7da052c2fc43512f28dd4afc698757ca0cce4386932fa6f"
EXPECTED_PINYIN_RAW_SHA256 = "e341598343a0f0f2035bb1aafc34a7f3bb7887deeecb3f60796262aaa2983e6b"
EXPECTED_PINYIN_DICT_SHA256 = "30b139bb7b3ca1f39bf05052820b4c6b2d124acc4c9bb67047a03170ed2b5f97"

def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while chunk := f.read(65536):
            h.update(chunk)
    return h.hexdigest().lower()

def ensure_file(path, url, expected_sha):
    if not os.path.exists(path):
        os.makedirs(os.path.dirname(path), exist_ok=True)
        print(f"Downloading {url} to {path}...")
        urllib.request.urlretrieve(url, path)
    actual_sha = sha256_file(path)
    if actual_sha != expected_sha.lower():
        raise ValueError(f"SHA256 mismatch for {path}: expected {expected_sha}, got {actual_sha}")
    print(f"Verified input {path} (SHA256: {actual_sha[:16]}...)")

def process_wubi():
    in_path = os.path.join("data", "dictionaries", "wubi86.raw.yaml")
    ensure_file(in_path, WUBI_URL, EXPECTED_WUBI_RAW_SHA256)
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
                norm_weight = min(1000, max(1, int(weight / 1000000))) if weight > 0 else 10
                f_out.write(f"{text}\t{code}\t{norm_weight}\n")
                count += 1
    out_sha = sha256_file(out_path)
    if out_sha != EXPECTED_WUBI_DICT_SHA256.lower():
        raise ValueError(f"Output SHA256 mismatch for {out_path}: expected {EXPECTED_WUBI_DICT_SHA256}, got {out_sha}")
    print(f"Processed Wubi86: {count} entries written to {out_path} (SHA256 verified)")
    return count

def process_pinyin():
    in_path = os.path.join("data", "dictionaries", "pinyin_simp.raw.yaml")
    ensure_file(in_path, PINYIN_URL, EXPECTED_PINYIN_RAW_SHA256)
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
                continuous_code = syllables_str.replace(" ", "")
                weight = 10
                if len(parts) >= 3 and parts[2].strip().isdigit():
                    weight = int(parts[2].strip())
                norm_weight = min(1000, max(1, weight))
                f_out.write(f"{text}\t{continuous_code}\t{syllables_str}\t{norm_weight}\n")
                count += 1
    out_sha = sha256_file(out_path)
    if out_sha != EXPECTED_PINYIN_DICT_SHA256.lower():
        raise ValueError(f"Output SHA256 mismatch for {out_path}: expected {EXPECTED_PINYIN_DICT_SHA256}, got {out_sha}")
    print(f"Processed Pinyin: {count} entries written to {out_path} (SHA256 verified)")
    return count

if __name__ == "__main__":
    wubi_count = process_wubi()
    pinyin_count = process_pinyin()
    print(f"Total entries: {wubi_count + pinyin_count} (Converter v{CONVERTER_VERSION})")

