#!/usr/bin/env python3
"""Собирает firmware/src/pinned_roots.h из firmware/certs/roots/*.pem.

Зачем генератор, а не ручная правка: 2026-11-04 ручная сборка набора
потеряла строку END CERTIFICATE у каждого корня, и все закреплённые узлы
отвалились разом. Здесь каждый PEM проверяется openssl-ом (разбирается,
срок действия не ближе трёх лет), счётчики BEGIN/END сверяются, и только
потом пишется заголовок.

Запуск: python3 firmware/certs/gen_pinned_roots.py
"""
import datetime
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent
OUT = ROOT.parent / "src" / "pinned_roots.h"
MIN_YEARS_LEFT = 3

HEADER = '''// Закреплённые корневые сертификаты для TLS — НАБОР крупных публичных CA,
// один и тот же для всех узлов (docs/decisions.md, п.9).
//
// Почему набор, а не корень на хост: за месяц imap.gmail.com переехал с GTS
// Root R1 на R4, chatgpt.com — с Google на Let's Encrypt, и почта с GPT
// пропали с панели (2026-11-04). Оператор CDN волен крутить корни внутри
// своего CA и менять сам CA без предупреждения; набор из основных семейств
// (Google, Let's Encrypt, DigiCert, GlobalSign, Amazon, Sectigo/USERTrust,
// Entrust, Microsoft, GoDaddy/Starfield) закрывает практически все публичные
// узлы до 2030-х. Общий бандл esp_crt_bundle не используется: его путь
// проверки стабильно ломался на корнях GTS (см. Status Log).
//
// setCACert() с этой строкой — обычный путь mbedTLS: полная проверка цепочки
// и hostname по списку из нескольких PEM (mbedtls_x509_crt_parse принимает
// их подряд).
//
// ФАЙЛ СГЕНЕРИРОВАН firmware/certs/gen_pinned_roots.py из firmware/certs/
// roots/*.pem — править PEM-файлы и перегенерировать, не этот заголовок.
#pragma once

'''


def check(pem_path: pathlib.Path) -> str:
    pem = pem_path.read_text()
    if pem.count("-----BEGIN CERTIFICATE-----") != 1 or pem.count("-----END CERTIFICATE-----") != 1:
        sys.exit(f"{pem_path.name}: ожидался ровно один сертификат")
    info = subprocess.run(
        ["openssl", "x509", "-noout", "-subject", "-enddate"], input=pem, capture_output=True, text=True
    )
    if info.returncode != 0:
        sys.exit(f"{pem_path.name}: openssl не разобрал PEM: {info.stderr.strip()}")
    end_line = [l for l in info.stdout.splitlines() if l.startswith("notAfter=")][0]
    end = datetime.datetime.strptime(end_line[len("notAfter="):], "%b %d %H:%M:%S %Y %Z")
    if end < datetime.datetime.now(datetime.timezone.utc).replace(tzinfo=None) + datetime.timedelta(days=365 * MIN_YEARS_LEFT):
        sys.exit(f"{pem_path.name}: истекает {end:%Y-%m-%d} — меньше {MIN_YEARS_LEFT} лет, замените корень")
    subject = [l for l in info.stdout.splitlines() if l.startswith("subject=")][0][len("subject="):]
    return f"{subject.strip()} · до {end:%Y-%m-%d}"


def main() -> None:
    pems = sorted((ROOT / "roots").glob("*.pem"))
    if not pems:
        sys.exit("firmware/certs/roots пуст")
    out = [HEADER]
    names = []
    for pem_path in pems:
        note = check(pem_path)
        macro = "ROOT_" + pem_path.stem.upper()
        names.append(macro)
        lines = pem_path.read_text().strip().splitlines()
        out.append(f"// {note}\n#define {macro} \\\n" + " \\\n".join(f'    "{l}\\n"' for l in lines) + "\n\n")
    out.append("constexpr const char* kPinnedRootsPem =\n    " + " \\\n    ".join(names) + ";\n")
    out.append(f"constexpr int kPinnedRootsCount = {len(names)};\n")
    text = "".join(out)
    assert text.count("BEGIN CERTIFICATE") == text.count("END CERTIFICATE") == len(pems)
    OUT.write_text(text)
    print(f"{OUT.relative_to(ROOT.parent.parent)}: {len(pems)} корней, {len(text)} байт")


if __name__ == "__main__":
    main()
