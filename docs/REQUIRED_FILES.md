# Arquivos originais necessários

O que o DKRecomp vai precisar, em cada fase, vindo da cópia do próprio usuário. Instruções de como obter esses
arquivos não são fornecidas aqui; o DKRecomp não decripta nada (`docs/LEGAL.md`).

## Já disponíveis e verificados

| Arquivo | Uso | Estado |
|---|---|---|
| `Donkey Kong Country Tropical Freeze [0100C1F0051B6000][v0].nsp` | Identificação do jogo base | Íntegro (SHA-256 de todos os conteúdos confere) |
| `Donkey Kong Country Tropical Freeze [0100C1F0051B6800][v131072].nsp` | Identificação da atualização | Íntegro |

Esses NSPs bastam para identificar e validar a versão. Eles **não** bastam para analisar ou recompilar o código,
porque o código está dentro de NCAs criptografados.

## Já fornecidos (2026-10-30)

- `user_files/exefs/` do **jogo base 1.0.0** (`main`, `main.npdm`, `rtld`, `sdk`, `subsdk0`), íntegra.
- `control.nacp` do jogo base (em `SwitchExtract/control/`).

Ainda faltam: a ExeFS e o `control.nacp` da atualização (para confirmar 1.0.2) e a RomFS combinada (MVP 5).

## Necessários para a Fase 3 (análise do executável) e seguintes

Coloque-os fora do repositório ou em `user_files/` (ignorada pelo Git). Prefira os da **versão que será o alvo**
(a atualização 131072 se ela for a versão final do jogo; o `main` costuma mudar entre versões).

| Arquivo / pasta | Conteúdo | Para quê | Fase |
|---|---|---|---|
| `exefs/main` | NSO com o código do jogo | Recompilação; análise de cobertura do lifter | 3, 4 |
| `exefs/main.npdm` | Metadados do programa | Serviços e SVCs permitidos, thread principal, espaço de endereços | 3 |
| `exefs/sdk`, `exefs/subsdk*` | NSOs do SDK da Nintendo e middleware | Fronteira com o sistema (`nn::*`), versão do SDK, NVN | 3, 5 |
| `exefs/rtld` | Carregador dinâmico | Referência de ligação entre módulos (o host deve substituí-lo) | 3 |
| `control.nacp` (do NCA Control) | Nomes e versão exibida | Confirmar se v131072 = "1.0.2" (hipótese H1) | 3 |
| `romfs/` (base + atualização já combinadas) | Dados do jogo | Execução (MVP 5+); o jogo lê esses arquivos | 5 |

Comando de análise quando a ExeFS existir:

```bash
.venv/Scripts/python -m dkrecomp.identify user_files/exefs --out reports/identify_exefs
```

(rodar com `PYTHONPATH=tools`, ver `docs/TESTING.md`).

## Nunca necessários

Chaves (`prod.keys`, `title.keys`), firmware do console, arquivos de outros jogos, saves de outras pessoas.
