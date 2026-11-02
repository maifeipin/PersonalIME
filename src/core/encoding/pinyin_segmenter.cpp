#include "pinyin_segmenter.h"
#include <algorithm>
#include <sstream>

namespace ime {

static const char* PINYIN_SYLLABLES[] = {
    "a", "ai", "an", "ang", "ao", "ba", "bai", "ban", "bang", "bao", "bei", "ben", "beng", "bi", "bian", "biao", "bie", "bin", "bing", "bo", "bu",
    "ca", "cai", "can", "cang", "cao", "ce", "cen", "ceng", "cha", "chai", "chan", "chang", "chao", "che", "chen", "cheng", "chi", "chong", "chou",
    "chu", "chua", "chuai", "chuan", "chuang", "chui", "chun", "chuo", "ci", "cong", "cou", "cu", "cuan", "cui", "cun", "cuo",
    "da", "dai", "dan", "dang", "dao", "de", "dei", "den", "deng", "di", "dia", "dian", "diao", "die", "ding", "diu", "dong", "dou", "du", "duan", "dui", "dun", "duo",
    "e", "ei", "en", "eng", "er", "fa", "fan", "fang", "fei", "fen", "feng", "fo", "fou", "fu",
    "ga", "gai", "gan", "gang", "gao", "ge", "gei", "gen", "geng", "gong", "gou", "gu", "gua", "guai", "guan", "guang", "gui", "gun", "guo",
    "ha", "hai", "han", "hang", "hao", "he", "hei", "hen", "heng", "hong", "hou", "hu", "hua", "huai", "huan", "huang", "hui", "hun", "huo",
    "ji", "jia", "jian", "jiang", "jiao", "jie", "jin", "jing", "jiong", "jiu", "ju", "juan", "jue", "jun",
    "ka", "kai", "kan", "kang", "kao", "ke", "kei", "ken", "keng", "kong", "kou", "ku", "kua", "kuai", "kuan", "kuang", "kui", "kun", "kuo",
    "la", "lai", "lan", "lang", "lao", "le", "lei", "leng", "li", "lia", "lian", "liang", "liao", "lie", "lin", "ling", "liu", "lo", "long", "lou", "lu", "luan", "lue", "lun", "luo", "lv", "lve",
    "ma", "mai", "man", "mang", "mao", "me", "mei", "men", "meng", "mi", "mian", "miao", "mie", "min", "ming", "miu", "mo", "mou", "mu",
    "na", "nai", "nan", "nang", "nao", "ne", "nei", "nen", "neng", "ni", "nia", "nian", "niang", "niao", "nie", "nin", "ning", "niu", "nong", "nou", "nu", "nuan", "nue", "nun", "nuo", "nv", "nve",
    "o", "ou", "pa", "pai", "pan", "pang", "pao", "pei", "pen", "peng", "pi", "pian", "piao", "pie", "pin", "ping", "po", "pou", "pu",
    "qi", "qia", "qian", "qiang", "qiao", "qie", "qin", "qing", "qiong", "qiu", "qu", "quan", "que", "qun",
    "ran", "rang", "rao", "re", "ren", "reng", "ri", "rong", "rou", "ru", "rua", "ruan", "rui", "run", "ruo",
    "sa", "sai", "san", "sang", "sao", "se", "sen", "seng", "sha", "shai", "shan", "shang", "shao", "she", "shei", "shen", "sheng", "shi", "shou", "shu", "shua", "shuai", "shuan", "shuang", "shui", "shun", "shuo", "si", "song", "sou", "su", "suan", "sui", "sun", "suo",
    "ta", "tai", "tan", "tang", "tao", "te", "tei", "teng", "ti", "tian", "tiao", "tie", "ting", "tong", "tou", "tu", "tuan", "tui", "tun", "tuo",
    "wa", "wai", "wan", "wang", "wei", "wen", "weng", "wo", "wu",
    "xi", "xia", "xian", "xiang", "xiao", "xie", "xin", "xing", "xiong", "xiu", "xu", "xuan", "xue", "xun",
    "ya", "yan", "yang", "yao", "ye", "yi", "yin", "ying", "yo", "yong", "you", "yu", "yuan", "yue", "yun",
    "za", "zai", "zan", "zang", "zao", "ze", "zei", "zen", "zeng", "zha", "zhai", "zhan", "zhang", "zhao", "zhe", "zhei", "zhen", "zheng", "zhi", "zhong", "zhou", "zhu", "zhua", "zhuai", "zhuan", "zhuang", "zhui", "zhun", "zhuo", "zi", "zong", "zou", "zu", "zuan", "zui", "zun", "zuo"
};

PinyinSegmenter::PinyinSegmenter() {
    for (const char* syl : PINYIN_SYLLABLES) {
        m_validSyllables.insert(syl);
    }
}

bool PinyinSegmenter::IsValidSyllable(const std::string& syl) const {
    return m_validSyllables.find(syl) != m_validSyllables.end();
}

void PinyinSegmenter::Backtrack(const std::string& s, size_t start,
                               std::vector<std::string>& current,
                               std::vector<std::vector<std::string>>& results) const {
    if (start == s.length()) {
        results.push_back(current);
        return;
    }

    // Try longest syllable match first (max pinyin syllable length is 6: chuang, zhuang, shuang)
    size_t maxLen = (std::min)(static_cast<size_t>(6), s.length() - start);
    for (size_t len = maxLen; len >= 1; --len) {
        std::string sub = s.substr(start, len);
        if (IsValidSyllable(sub)) {
            current.push_back(sub);
            Backtrack(s, start + len, current, results);
            current.pop_back();
        }
    }
}

std::vector<PinyinSegmentation> PinyinSegmenter::Segment(const std::string& input) const {
    std::vector<PinyinSegmentation> result;
    if (input.empty()) return result;

    std::string clean = input;
    std::transform(clean.begin(), clean.end(), clean.begin(), [](unsigned char c) { return std::tolower(c); });

    bool hasSep = (clean.find('\'') != std::string::npos);

    if (hasSep) {
        // Explicit apostrophe: e.g. "xi'an", "fang'an"
        std::vector<std::string> parts;
        std::stringstream ss(clean);
        std::string item;
        while (std::getline(ss, item, '\'')) {
            if (!item.empty()) {
                parts.push_back(item);
            }
        }

        // Each part must be segmented
        std::vector<std::vector<std::string>> combined = {{}};
        for (const auto& part : parts) {
            std::vector<std::string> cur;
            std::vector<std::vector<std::string>> segs;
            Backtrack(part, 0, cur, segs);
            if (segs.empty()) {
                // If subpart is not fully segmented, allow it as a literal segment
                segs.push_back({part});
            }

            std::vector<std::vector<std::string>> nextCombined;
            for (const auto& prior : combined) {
                for (const auto& seg : segs) {
                    std::vector<std::string> merged = prior;
                    merged.insert(merged.end(), seg.begin(), seg.end());
                    nextCombined.push_back(std::move(merged));
                }
            }
            combined = std::move(nextCombined);
        }

        for (const auto& seq : combined) {
            if (seq.empty()) continue;
            PinyinSegmentation pseg;
            pseg.syllables = seq;
            pseg.has_explicit_separator = true;
            std::string continuous;
            std::string spaced;
            for (size_t i = 0; i < seq.size(); ++i) {
                continuous += seq[i];
                if (i > 0) spaced += " ";
                spaced += seq[i];
            }
            pseg.raw_continuous = continuous;
            pseg.spaced_syllables = spaced;
            result.push_back(std::move(pseg));
        }
    } else {
        // Continuous input: e.g. "xian", "zhongwen"
        std::vector<std::string> cur;
        std::vector<std::vector<std::string>> segs;
        Backtrack(clean, 0, cur, segs);

        for (const auto& seq : segs) {
            PinyinSegmentation pseg;
            pseg.syllables = seq;
            pseg.has_explicit_separator = false;
            pseg.raw_continuous = clean;
            std::string spaced;
            for (size_t i = 0; i < seq.size(); ++i) {
                if (i > 0) spaced += " ";
                spaced += seq[i];
            }
            pseg.spaced_syllables = spaced;
            result.push_back(std::move(pseg));
        }

        // If continuous string could not be cleanly partitioned into syllables,
        // still preserve the raw continuous string for prefix queries
        if (result.empty()) {
            PinyinSegmentation pseg;
            pseg.syllables = {clean};
            pseg.has_explicit_separator = false;
            pseg.raw_continuous = clean;
            pseg.spaced_syllables = clean;
            result.push_back(std::move(pseg));
        }
    }

    return result;
}

} // namespace ime

