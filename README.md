# DKRecomp

Ferramenta de **recompilação estática** que, a longo prazo, deve converter a versão Nintendo Switch de
*Donkey Kong Country: Tropical Freeze* num programa nativo para Windows x86-64, sem emulador durante a execução.

> **O jogo não roda.** O projeto está no início: ferramentas, identificação de arquivos e um recompilador
> ARM64 → C validado em programas de teste próprios. Veja o estado real em [docs/PROGRESS.md](docs/PROGRESS.md).

> **Nenhum arquivo do jogo está incluído**, e o DKRecomp não decripta nem baixa nada. Você precisa da sua própria
> cópia legítima. Veja [docs/LEGAL.md](docs/LEGAL.md) e [docs/REQUIRED_FILES.md](docs/REQUIRED_FILES.md).

## O que já funciona

- `dkrecomp.identify`: lê NSPs (contêiner PFS0, ticket, metadados), confere o SHA-256 de cada conteúdo e reconhece
  a versão; lê ExeFS extraídas (NSO, NPDM, imports, SVCs, middleware).
- Decodificador e recompilador ARM64 → C (inteiros, desvios, loads/stores, atômicos exclusivos, movimentação
  SIMD/FP), com runtime mínimo em C.
- Testes diferenciais: o mesmo código ARM64 roda no Unicorn e recompilado, e precisa dar o mesmo resultado
  (dezenas de milhares de instruções aleatórias e programas C inteiros em O0/O1/O2/Os).

## Começar

Requisitos: Windows 10/11 x86-64, Python 3.13, Git.

```bash
python -m venv .venv
.venv/Scripts/python -m pip install -r requirements-dev.txt
```

```bash
.venv/Scripts/python -m pytest
```

Identificar seus arquivos (relatório em `reports/`, ignorado pelo Git):

```bash
PYTHONPATH=tools .venv/Scripts/python -m dkrecomp.identify "game original meu" --hash
```

Mais comandos em [docs/TESTING.md](docs/TESTING.md).

## Documentação

| Documento | Conteúdo |
|---|---|
| [docs/FEASIBILITY.md](docs/FEASIBILITY.md) | Viabilidade: fatos, hipóteses, incógnitas, comparação com o TotK Recomp |
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | Modelo de execução e organização do código |
| [docs/ROADMAP.md](docs/ROADMAP.md) | Marcos (MVP 1–6) e critérios de aceitação |
| [docs/PROGRESS.md](docs/PROGRESS.md) | Progresso real, com evidências |
| [docs/TESTING.md](docs/TESTING.md) | Como testar e o que cada teste prova |
| [docs/KNOWN_ISSUES.md](docs/KNOWN_ISSUES.md) | Limitações e bloqueios |
| [docs/LEGAL.md](docs/LEGAL.md) | Regras sobre arquivos proprietários |
| [docs/REQUIRED_FILES.md](docs/REQUIRED_FILES.md) | Arquivos do jogo necessários em cada fase |
| [docs/REFERENCES_AUDIT.md](docs/REFERENCES_AUDIT.md) | Auditoria de confiabilidade dos repositórios sugeridos |

## Créditos e licença

Projetos estudados como referência (nenhum código copiado até agora): TotK Recomp, LAS Recomp, SwitchRecomp,
N64Recomp. Ferramentas de terceiros: [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

Licença do código do DKRecomp: **a definir pelo mantenedor** (MIT é a opção compatível com os projetos de
referência). Nenhuma licença do projeto cobre o jogo.

Projeto independente, sem afiliação com a Nintendo ou a Retro Studios. *Donkey Kong Country*, *Tropical Freeze* e
*Nintendo Switch* são marcas da Nintendo.
