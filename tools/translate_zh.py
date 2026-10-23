#!/usr/bin/env python3
"""Build offline Chinese catalogue drafts; model weights stay in build/.

Requires CTranslate2, SentencePiece and OpenCC. No network calls are made.
Reviewed terminology in lang/zh-overrides.json overrides model output.
"""
import argparse
import json
from pathlib import Path
import re
import sys

import ctranslate2
from opencc import OpenCC
import sentencepiece as sp

import gen_lang

ROOT = Path(__file__).resolve().parents[1]
# Keep formatting, escape sequences, markup and URLs out of translation.
TOKEN = re.compile(r'(%(?:\d+\$)?[-+ #0]*(?:\*|\d+)?(?:\.(?:\*|\d+))?(?:hh|ll|[hlzjtL])?[diuoxXfFeEgGaAcsp%](?=$|[^a-z]|x%)|\\(?:x[0-9a-fA-F]{2}|[ntr"\\])|<[^>]+>|https?://[^\s]+|\b[A-Za-z_][\w.-]*\.(?:txt|csv|json|bin|so|pak|png|jpg|py|lua|js|html|zip)\b|/(?:[\w.-]+/)*[\w.-]+|\b(?:ESP32(?:-[A-Za-z0-9]+)*|GPIO\d*|I2C|SPI|UART|PSRAM|USB(?:-OTG)?|Modbus|Home Assistant|Frigate|Riden|HTTPS?|MQTT|LVGL|RTS|DTR|BOOT|EN|SDA|SCL|VCC|VDD|GND|MOSI|MISO|CS|VREF|RST|TTL|OTA|NVS|FIFO|ADC|DAC|PWM|RTC|RGB\d*|CAN|BLE|Wi-Fi|[A-Z]{2,}\d+[A-Z0-9]*)\b)')


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--engine', choices=('nllb', 'marian'), default='nllb')
    ap.add_argument('--model', type=Path)
    ap.add_argument('--batch-size', type=int, default=24)
    ap.add_argument('--tokenizers', type=Path, default=ROOT / 'build/chinese-source/model')
    args = ap.parse_args()
    if args.model is None:
        args.model = ROOT / ('build/chinese-source/nllb' if args.engine == 'nllb' else 'build/chinese-source/ct2')
    cache_file = ROOT / ('build/chinese-source/nllb-translations.json' if args.engine == 'nllb' else 'build/chinese-source/translations.json')
    cache = json.loads(cache_file.read_text(encoding='utf-8')) if cache_file.exists() else {}
    overrides = json.loads((ROOT / 'lang/zh-overrides.json').read_text(encoding='utf-8'))
    packs = {p.name: gen_lang.read_pack(str(p)) for p in sorted((ROOT / 'lang/en').glob('*.lang'))}
    keyed = json.loads((ROOT / 'lang/zh-key-overrides.json').read_text(encoding='utf-8'))
    current = gen_lang.collect()
    packs = {name: {key: value for key, value in entries.items() if key in current.get(name, {})}
             for name, entries in packs.items()}
    def parts(value):
        return TOKEN.split(value)
    def needs(text):
        return bool(re.search('[A-Za-z]{2}', text))
    pending = sorted({piece.strip() for filename, entries in packs.items() for key, value in entries.items()
                      for i, piece in enumerate(parts(value)) if key not in keyed.get(filename, {}) and value not in overrides and i % 2 == 0 and needs(piece)
                      and piece.strip() not in cache and piece.strip() not in overrides}, key=len)
    model = ctranslate2.Translator(str(args.model), device='cpu', compute_type='int8', intra_threads=4)
    if args.engine == 'nllb':
        source = target = sp.SentencePieceProcessor(model_file=str(args.model / 'sentencepiece.bpe.model'))
    else:
        source = sp.SentencePieceProcessor(model_file=str(args.tokenizers / 'source.spm'))
        target = sp.SentencePieceProcessor(model_file=str(args.tokenizers / 'target.spm'))
    for start in range(0, len(pending), args.batch_size):
        batch = pending[start:start + args.batch_size]
        if args.engine == 'nllb':
            tokens = [['eng_Latn'] + source.encode(x, out_type=str) + ['</s>'] for x in batch]
            results = model.translate_batch(tokens, target_prefix=[['zho_Hans']] * len(tokens),
                                            beam_size=2, max_decoding_length=192, repetition_penalty=1.1)
        else:
            tokens = [['>>cmn_Hans<<'] + source.encode(x, out_type=str) for x in batch]
            results = model.translate_batch(tokens, beam_size=2, max_decoding_length=192, repetition_penalty=1.1)
        for text, result in zip(batch, results):
            hypothesis = result.hypotheses[0][1:] if args.engine == 'nllb' else result.hypotheses[0]
            cache[text] = target.decode(hypothesis).strip()
        cache_file.write_text(json.dumps(cache, ensure_ascii=False, indent=2), encoding='utf-8')
        print(f'{min(start + args.batch_size, len(pending))}/{len(pending)} translated locally', flush=True)
    keyed = json.loads((ROOT / 'lang/zh-key-overrides.json').read_text(encoding='utf-8'))
    baseline = json.loads((ROOT / 'build/chinese-source/translations.json').read_text(encoding='utf-8')) if (ROOT / 'build/chinese-source/translations.json').exists() else {}
    review = {}
    simplified = OpenCC('t2s')
    traditional = OpenCC('s2twp')
    traditional_keyed = json.loads((ROOT / 'lang/zh-Hant-key-overrides.json').read_text(encoding='utf-8'))
    def translate(value):
        if value in overrides:
            return overrides[value]
        out = []
        for i, piece in enumerate(parts(value)):
            if i % 2 or not needs(piece):
                out.append(piece)
            else:
                core = piece.strip()
                translated = overrides.get(core, cache.get(core, core))
                def invalid(text):
                    return '⁇' in text or '�' in text or len(text) > max(120, len(core) * 3)
                if invalid(translated):
                    alternate = baseline.get(core, core)
                    translated = alternate if not invalid(alternate) else core
                    review[core] = translated
                out.append((' ' if piece.startswith(' ') else '') + translated + (' ' if piece.endswith(' ') else ''))
        return simplified.convert(''.join(out))
    for code, name, convert in [('zh-Hans', '简体中文', lambda s: s), ('zh-Hant', '繁體中文', traditional.convert)]:
        folder = ROOT / 'lang' / code
        folder.mkdir(exist_ok=True)
        (folder / 'meta.txt').write_text('name=' + name + '\n', encoding='utf-8')
        for filename, entries in packs.items():
            lines = ['# Offline Chinese catalogue; reviewed terms: ../zh-overrides.json',
                     '# Spanish keys and printf/escape tokens are preserved.']
            for key, value in entries.items():
                translated = keyed.get(filename, {}).get(key)
                if translated is None:
                    translated = translate(value)
                # Hardware text uses board/card/dock in their device context.
                if filename == '_sistema.lang' or filename == '_portal.lang' or filename.startswith(('aos.', 'web.')):
                    if re.search(r'\bboard\b', value, re.I):
                        for wrong in ('董事会', '委员会', '棋盘'):
                            translated = translated.replace(wrong, '开发板')
                    if re.search(r'\bcard\b', value, re.I):
                        translated = translated.replace('名片', '存储卡').replace('卡片', '存储卡')
                    if re.search(r'\bdock\b', value, re.I):
                        translated = translated.replace('码头', '程序坞').replace('船坞', '程序坞')
                translated = convert(translated)
                if code == 'zh-Hant':
                    translated = traditional_keyed.get(filename, {}).get(key, translated)
                translated = translated.replace('\t', ' ').replace('\r', '').replace('\n', '\\n')
                lines.append(key + '\t' + translated)
            (folder / filename).write_text('\n'.join(lines) + '\n', encoding='utf-8')
        print(code, len(packs), 'catalogues written', flush=True)
    (ROOT / 'build/chinese-source/review-fallbacks.json').write_text(json.dumps(review, ensure_ascii=False, indent=2), encoding='utf-8')
    print(len(review), 'model fallbacks listed in build/chinese-source/review-fallbacks.json', flush=True)


if __name__ == '__main__':
    main()
