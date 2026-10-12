#!/usr/bin/env python3
import argparse
import hashlib
import json
from pathlib import Path

def sha256_file(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def load_json(path):
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except Exception as exc:
        raise SystemExit(f"ERRO: não foi possível ler {path}: {exc}")

def save_json(path, data):
    path.write_text(
        json.dumps(data, indent=2, ensure_ascii=False) + "\n",
        encoding="utf-8",
        newline="\n",
    )

def main():
    parser = argparse.ArgumentParser(
        description=(
            "Verifica ou atualiza os SHA-256 dos arquivos já registrados "
            "em PUBLIC_SOURCE_GOLD_HASHES.json."
        )
    )
    parser.add_argument(
        "--root",
        help="Diretório mk64-master. Detectado automaticamente por padrão.",
    )
    parser.add_argument(
        "--json",
        help="Caminho do PUBLIC_SOURCE_GOLD_HASHES.json.",
    )
    parser.add_argument(
        "--check",
        action="store_true",
        help="Somente verifica; não altera o JSON.",
    )
    parser.add_argument(
        "--update",
        action="store_true",
        help="Atualiza os hashes que mudaram.",
    )
    args = parser.parse_args()

    if args.check and args.update:
        raise SystemExit("ERRO: use --check ou --update, não os dois.")

    json_name = "PUBLIC_SOURCE_GOLD_HASHES.json"

    if args.root:
        root = Path(args.root).expanduser().resolve()
    elif args.json:
        root = Path(args.json).expanduser().resolve().parent
    else:
        # Funciona tanto na pasta mk64-master quanto na pasta pai.
        candidates = [
            Path.cwd(),
            Path(__file__).resolve().parent,
            Path.cwd() / "mk64-master",
            Path(__file__).resolve().parent / "mk64-master",
        ]
        root = next(
            (
                candidate.resolve()
                for candidate in candidates
                if (candidate.resolve() / json_name).is_file()
            ),
            None,
        )
        if root is None:
            raise SystemExit(
                f"ERRO: não foi possível localizar {json_name}. "
                "Execute na pasta do projeto ou informe --root."
            )

    json_path = (
        Path(args.json).expanduser().resolve()
        if args.json
        else root / json_name
    )

    if not json_path.is_file():
        raise SystemExit(f"ERRO: JSON não encontrado: {json_path}")

    hashes = load_json(json_path)

    if not isinstance(hashes, dict):
        raise SystemExit("ERRO: PUBLIC_SOURCE_GOLD_HASHES.json deve conter um objeto JSON.")

    missing = []
    changed = []
    unchanged = []

    for rel_path, old_hash in hashes.items():
        path = root / rel_path

        if not path.is_file():
            missing.append(rel_path)
            continue

        new_hash = sha256_file(path)

        if new_hash == old_hash:
            unchanged.append(rel_path)
        else:
            changed.append((rel_path, old_hash, new_hash))

    print("PUBLIC_SOURCE_GOLD_HASHES")
    print("=" * 72)
    print(f"Root: {root}")
    print(f"JSON: {json_path}")
    print(f"Arquivos registrados: {len(hashes)}")
    print()

    if missing:
        print("ARQUIVOS AUSENTES:")
        for rel_path in missing:
            print(f"  {rel_path}")
        print()

    if changed:
        print("HASHES DIFERENTES:")
        for rel_path, old_hash, new_hash in changed:
            print(f"  {rel_path}")
            print(f"    antigo: {old_hash}")
            print(f"    atual:  {new_hash}")
        print()

    print(f"Sem alteração: {len(unchanged)}")
    print(f"Com alteração: {len(changed)}")
    print(f"Ausentes:      {len(missing)}")

    if args.update:
        if missing:
            print()
            print("ERRO: existem arquivos ausentes. O JSON não será alterado.")
            return 1

        for rel_path, old_hash, new_hash in changed:
            hashes[rel_path] = new_hash

        if changed:
            save_json(json_path, hashes)
            print()
            print(f"Atualizados {len(changed)} hash(es) em:")
            print(f"  {json_path}")
        else:
            print()
            print("Nenhum hash precisa ser atualizado.")

        return 0

    if changed:
        print()
        print("Os hashes acima precisam ser atualizados se as alterações forem intencionais.")
        print("Use:")
        print("  py UPDATE_PUBLIC_SOURCE_GOLD_HASHES.py --update")
        return 2

    if missing:
        return 1

    return 0

if __name__ == "__main__":
    raise SystemExit(main())
