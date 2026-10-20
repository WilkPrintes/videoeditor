# VideoEditor

Editor de vídeo não linear **nativo** para Windows e macOS, em C++20 com Qt 6 (Widgets e viewer
desenhado na GPU via Qt RHI), FFmpeg (LGPL, linkagem dinâmica) e miniaudio. "VideoEditor" é um
nome provisório.

É um **caso de estudo** para validar se é possível construir ferramentas de edição do porte das
de um editor profissional. Não tem nenhum recurso de monetização: nada de licença, conta, loja ou
telemetria. A especificação completa, com as fases 0–10 e os testes de aceitação, está em
[docs/ESPECIFICACAO.md](docs/ESPECIFICACAO.md).

A arquitetura segue padrões observados no DaVinci Resolve (análise estática): modelo de dados
separado da UI, edições como *Commands* com desfazer, motor de reprodução em threads próprias com
filas e processamento de imagem em float na GPU. Nenhum código do Resolve foi usado. Os detalhes
estão em [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

## Estado: fases 1 e 2 prontas, fase 3 em andamento

Tudo verificado no Windows 11 (RTX 3060). O macOS ainda não foi compilado.

### Fase 1: reproduzir e exportar (marco 1)

| Critério | Situação |
|---|---|
| Compila e abre | ✅ MSVC 14.51 (VS Build Tools 2026), Qt 6.12.0, FFmpeg 9.0 LGPL |
| Importa vídeo | ✅ menu, Ctrl+I ou arrastar arquivos; o primeiro vídeo define fps e resolução da timeline |
| Cortar, mover, trim | ✅ blade (Ctrl+B), mover por arraste, trim pelas bordas, apagar, desfazer/refazer |
| Reprodução com áudio sincronizado | ✅ o áudio é o relógio mestre; decodificação D3D11VA; desvio imagem/relógio de 0 frame medido |
| Exporta H.264/MP4 | ✅ H.264 + AAC; frames e áudio conferidos frame a frame contra a timeline |

### Fase 2: editor de verdade (marco 2, §6.1–6.3)

| Critério | Situação |
|---|---|
| Multitrilha | ✅ trilhas de vídeo e áudio sem limite, com rolagem vertical. Arrastar um clipe para cima ou para baixo troca de trilha. O cabeçalho de cada trilha escolhe o destino das edições e liga a trava e o sync lock |
| Edição em 3 e 4 pontos | ✅ inserir (F9), sobrescrever (F10), substituir (F11), fit to fill (Shift+F11, muda a velocidade) e colocar por cima (F12, cria trilhas). Usa o In/Out da timeline (I/O) e o playhead |
| Trims | ✅ ripple, roll, slip e slide, pela ferramenta Trim (T) ou por modificador. Com Alt, só a trilha clicada é ajustada (cortes J e L). Ripple delete com Shift+Delete |
| Trava e sync lock | ✅ trilhas travadas nunca mudam. Inserts e ripples deslocam também as trilhas com sync lock, e imagem e som de um take continuam em sincronia (veja [as regras](docs/ARCHITECTURE.md#edições-da-fase-2)) |
| Clipes compostos | ✅ compor (Alt+C) e decompor (Alt+Shift+C) devolvem o mesmo estado. O pai renderiza a timeline filha com o mesmo hash de frames e de amostras do original |
| Marcadores e snapping | ✅ M adiciona um marcador, arrastar no ruler move e Delete apaga. Bordas, clipes e marcadores grudam em cortes, marcadores, playhead e In/Out (N liga e desliga) |
| Desfazer e transações | ✅ cada arraste é uma transação editada ao vivo (Esc cancela) e vira um único passo de desfazer |

### Fase 3: página Edit completa e interface (em andamento, §12.1)

| Passo | Situação |
|---|---|
| 3.1 Organização | ✅ cores de clipe e de trilha, flags, nomes, ativar e desativar clipes e trilhas, marcadores com nome, nota, cor e duração, link/unlink, linked selection (Ctrl+Shift+L) |
| 3.2 Edições que faltam | ✅ append (Shift+F12), ripple overwrite (Shift+F10), inserir gap, apagar gaps, trocar clipes de lugar e várias timelines em abas (nova, duplicar, renomear, apagar). Um duplo clique num clipe composto abre a timeline dele |
| 3.3 Painéis | ✅ media pool, edit index, viewer de fonte, inspector e timeline são painéis acopláveis: mostrar e esconder (menu Workspace), mover, destacar para outro monitor. Presets de layout com nome; o último layout volta ao abrir |
| 3.4 Viewer de fonte | ✅ o duplo clique no pool abre a mídia num viewer com transporte e In/Out próprios. O viewer ativo (borda amarela; Q alterna) recebe Espaço, K/L, setas, Home/End e I/O, e F9–F12 usam o In/Out da fonte |
| 3.5 Inspector, edit index e menus | ✅ inspector da timeline, do clipe, do marcador e da trilha; edit index com as edições e os marcadores; menus de contexto no clipe, no gap, no cabeçalho, no marcador e no ruler |
| 3.6 Trim por teclado e atalhos | a fazer |

### Como foi verificado

- `ve_core_tests` (Catch2, 79 casos e 1 024 asserções):
  - tempo em flicks, JSON, arquivo de projeto, comandos e transações;
  - cada edição das fases 2 e 3, inclusive com trilhas travadas, sem sync lock e com linked
    selection desligado;
  - **29 estados dourados** em `tests/core/golden/*.json` (os 22 da fase 2, inalterados, e 7 da
    fase 3): o antes e o depois de cada operação via `timelineToJson`, mais um resumo legível em
    frames;
  - **teste de propriedade** em duas rodadas de 22 500 edições aleatórias (cinco sementes de 4 500)
    de todos os tipos, inclusive append, ripple overwrite, gaps, swap e link/unlink, com mais de
    10 000 trims aplicados no total. A cada passo confere `edit::checkInvariants` (grade de
    frames, nenhuma mídia fora da fonte, nenhuma sobreposição, ids únicos), o alinhamento V/A e se
    as trilhas travadas ficaram intactas. Na rodada só com edições ligadas, o alinhamento é estrito;
    na rodada com J/L, mídia só de vídeo ou só de áudio e travas, confere a sincronia. No fim, o undo
    total volta ao projeto inicial.
- `ve_ui_tests` (Qt Test), que dirige a janela real com mouse e teclado:
  - fase 1: importar → cortar → arrastar ao vivo → trim → arraste cancelado com Esc → desfazer →
    tocar;
  - fase 2: adicionar trilhas → ripple com a ferramenta Trim → ripple cancelado → roll (Ctrl) →
    corte L (Ctrl+Alt) → slip → mover para V2 → snapping em marcador → travar pelo cabeçalho →
    inserir do media pool (F9);
  - fase 3, painéis: docks no layout padrão → preset salvo, layout restaurado e preset carregado
    (com um painel escondido e outro flutuando) → duplo clique no pool abre o viewer de fonte →
    I/O e Espaço no viewer ativo → overwrite e append do trecho da fonte → abas de timeline, cada
    uma com o seu playhead;
  - fase 3, organização: renomear, colorir e desativar pelo inspector → cor, flag e unlink pelo
    menu de contexto → ripple delete de um gap → menus do cabeçalho, do marcador e do ruler → linha
    do edit index → linked selection desligado → clipe composto aberto com duplo clique.
- `ve-cli selftest` e `ve-cli play`: veja [Ferramentas de teste](#ferramentas-de-teste). O
  selftest exporta a timeline da fase 2 antes e depois de compor tudo num clipe composto e compara
  o hash de cada frame e amostra renderizados.

Já previsto na base para as próximas fases (§4–5 da especificação):
- **Tempo em flicks** (1/705 600 000 s): divide exatamente todas as cadências comuns e as taxas
  de áudio.
- **Modelo do §5.2**: pastas no media pool, parâmetros animáveis, efeitos, grade, retime, clipes
  compostos e multicam, trilhas com trava e sync lock, marcadores.
- **Commands com transações e fusão de passos** (§5.3).

## Estrutura

Segue o §4 da especificação. Cada camada só depende das de baixo:
`core ← media ← audio ← engine ← ui ← app`.

```
core/         tempo (flicks), modelo, edições, Commands/transações, JSON, arquivo .veproj,
              snapshot imutável para o motor                                (C++ puro: sem Qt, sem FFmpeg)
media/        FFmpeg: probe, decodificadores (D3D11VA/VideoToolbox), escrita MP4/H.264,
              thread de encoder (COM MTA), clipe de teste "claquete"        (sem Qt)
audio/        saída de áudio (miniaudio) e mixer da timeline                (sem Qt)
engine/       reprodução (relógio no áudio, decodificação antecipada, scrub) e exportação (sem Qt)
gpu/shaders/  GLSL 440 → .qsb (HLSL/MSL/SPIR-V) pelo qsb no build
ui/           janela com painéis acopláveis, viewers (timeline e fonte) em QRhiWidget, timeline,
              media pool, inspector, edit index, cores de etiqueta, export
app/          main.cpp
tools/ve-cli/ probe, export, auto-teste e teste de reprodução sem UI
tests/core/   Catch2: tempo, JSON, edições, 3/4 pontos, trims, compostos, organização, gaps,
              comandos, projeto, propriedade; golden/ com os estados dourados em JSON
tests/ui/     Qt Test: testes de GUI
fx/ color/ analysis/   reservadas para as fases 4, 5 e 9
scripts/      build.ps1, fetch-deps-windows.ps1, fetch_qt.py
third_party/  miniaudio 0.11.25 (domínio público / MIT-0)
```

## Dependências e licenças

| Componente | Versão usada | Licença | Observação |
|---|---|---|---|
| Visual Studio Build Tools (MSVC, Windows SDK, CMake, Ninja) | 2026 / 18.11 | Microsoft, nos termos do VS Community | só para compilar no Windows |
| Qt (qtbase, qtshadertools, qttools) | 6.12.0 | LGPLv3; a ferramenta `qsb` é GPLv3, mas só roda no build | o viewer usa `QRhi` (GuiPrivate), o que amarra o build à versão exata do Qt |
| FFmpeg (libav*) | 9.0, build "lgpl-shared" da BtbN | LGPL (essa build é LGPLv3) | sem x264; o H.264 sai pelos encoders de hardware ou pelo OpenH264 |
| miniaudio | 0.11.25 | domínio público ou MIT-0 | vendorizado em `third_party/` |
| Catch2 | 3.16.1 (amalgamado) | BSL-1.0 | só nos testes; vendorizado em `third_party/catch2` |

## Build no Windows

1. **Compilador** (precisa de admin; instala MSVC, Windows SDK, CMake e Ninja):
   ```powershell
   winget install --id Microsoft.VisualStudio.BuildTools -e --override "--wait --passive --norestart --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
   ```
2. **FFmpeg e Qt.** O script baixa o FFmpeg LGPL para `external/ffmpeg` e confere o SHA-256.
   Com `-Qt`, baixa também o Qt 6.12.0 para `C:\Qt`, conferindo o SHA-1 de cada pacote. Em vez
   do `-Qt`, você pode usar o instalador oficial do Qt: selecione "MSVC 2022 64-bit" e
   "Qt Shader Tools".
   ```powershell
   ./scripts/fetch-deps-windows.ps1 -Qt
   ```
3. **Compilar e testar.** O script carrega sozinho o ambiente do Visual Studio:
   ```powershell
   ./scripts/build.ps1 -Test
   ```
4. **Rodar:**
   ```powershell
   ./build/windows-release/videoeditor.exe
   ```

Se o Qt estiver em outro lugar, crie um `CMakeUserPresets.json` (ignorado pelo git):

```json
{
  "version": 6,
  "configurePresets": [
    { "name": "mine", "inherits": "windows-release",
      "cacheVariables": { "CMAKE_PREFIX_PATH": "D:/Qt/6.12.0/msvc2022_64" } }
  ],
  "buildPresets": [ { "name": "mine", "configurePreset": "mine" } ]
}
```

O preset `windows-debug` também funciona, porque o pacote MSVC do Qt traz as DLLs de debug.

## Build no macOS

> Não testado: não havia Mac disponível. O código já tem os caminhos de macOS (Metal no viewer,
> VideoToolbox para decodificar e codificar, Core Audio pelo miniaudio), mas a primeira
> compilação pode exigir pequenos ajustes.

### Apple Silicon com Homebrew (caminho mais simples)

```bash
xcode-select --install
brew install cmake ninja pkgconf qt ffmpeg
cmake --preset macos-release
cmake --build --preset macos-release
ctest --preset macos-release
open build/macos-release/videoeditor.app
```

O preset `macos-release` procura o Qt em `/opt/homebrew`. O FFmpeg é encontrado pelo
`pkg-config`. Para conferir o motor sem UI:

```bash
./build/macos-release/ve-cli make-test-clip /tmp/pattern.mp4 10 1920 1080 30
./build/macos-release/ve-cli selftest /tmp/pattern.mp4 /tmp/out.mp4
./build/macos-release/ve-cli play /tmp/pattern.mp4 5
```

### Mac Intel ou Qt do instalador oficial

A Homebrew não tem bottles do Qt para Intel. Nesse caso:
1. Instale o Qt 6.12 pelo instalador oficial: componente "macOS" mais "Qt Shader Tools".
2. Rode `brew install cmake ninja pkgconf ffmpeg`.
3. Configure apontando para o Qt:
   ```bash
   cmake --preset macos-release -DCMAKE_PREFIX_PATH=$HOME/Qt/6.12.0/macos
   ```

### Distribuir o .app

- `macdeployqt build/macos-release/videoeditor.app` copia os frameworks do Qt e as dylibs do
  FFmpeg para dentro do bundle. Para outros Macs abrirem o app, ainda falta assinar
  (`codesign`) e notarizar.
- **Licença:** o FFmpeg da Homebrew é **GPL** (vem com x264). Para distribuir sob LGPL, compile
  um FFmpeg próprio e passe `FFMPEG_ROOT=<prefixo>` ao CMake:
  ```bash
  ./configure --enable-shared --disable-static --disable-gpl --disable-nonfree --enable-videotoolbox --enable-audiotoolbox
  ```
  No Mac, o H.264 sai pelo `h264_videotoolbox`.

## Uso

| Ação | Atalho |
|---|---|
| Importar mídia | Ctrl+I, ou arrastar arquivos para o media pool ou para a timeline |
| Abrir mídia no viewer de fonte | duplo clique no media pool |
| Colocar mídia do pool | arrastar para a timeline (sobrescreve na posição) ou Shift+F12 (no fim) |
| Viewer ativo | clicar no viewer de fonte ou no da timeline (ou na timeline); Q alterna |
| Tocar/pausar | Espaço; L toca, K pausa (no viewer ativo) |
| Frame anterior/próximo | ← / →; com Shift, ± 1 segundo |
| Início / fim | Home / End |
| Cortar no playhead (blade) | Ctrl+B ou B (com clipes selecionados, corta só eles) |
| Ferramentas | A: seleção; T: trim |
| Mover clipe (seleção) | arrastar o corpo; vídeo e áudio ligados andam juntos; para cima ou para baixo troca de trilha |
| Trim na borda (seleção) | arrastar a borda: o clipe cresce ou encolhe só em espaço vazio |
| Ripple | ferramenta Trim + borda, ou Shift + borda: o que vem depois acompanha |
| Roll | Ctrl + borda: move o corte entre dois clipes |
| Slip / slide | ferramenta Trim + corpo (slip) ou Shift + corpo (slide) |
| Só esta trilha (J/L) | Alt junto com qualquer trim |
| Apagar seleção | Delete / Backspace (deixa um gap; com um marcador selecionado, apaga o marcador) |
| Ripple delete | Shift+Delete (fecha o buraco nas trilhas com sync lock) |
| Inserir / sobrescrever / substituir | F9 / F10 / F11, com a mídia e o In/Out do viewer de fonte (com ele vazio, a mídia selecionada no pool, inteira) |
| Fit to fill / colocar por cima | Shift+F11 / F12 |
| Append / ripple overwrite | Shift+F12 / Shift+F10 |
| In / Out | I / O no viewer ativo (fonte ou timeline); Alt+X limpa |
| Linked selection | Ctrl+Shift+L: desligado, o clique pega só a imagem ou só o som |
| Link / unlink | Ctrl+Alt+L |
| Cor, flags, nome | menu Clip, botão direito no clipe ou inspector; F2 renomeia; D ativa e desativa |
| Trocar clipe de lugar | Ctrl+Shift+, / Ctrl+Shift+. |
| Gaps | botão direito no gap: ripple delete; menu Timeline: inserir gap (In→Out ou 1 s), apagar todos |
| Timelines | Ctrl+Alt+N cria; abas acima das trilhas; botão direito na aba: renomear, duplicar, apagar |
| Painéis e layouts | menu Workspace: mostrar e esconder painéis, presets de layout, restaurar o padrão |
| Destino das edições, trava, sync lock | clicar no nome da trilha, no cadeado ou na corrente do cabeçalho |
| Trilhas | Ctrl+Alt+V adiciona vídeo, Ctrl+Alt+A adiciona áudio; Shift+roda rola as trilhas |
| Marcadores | M adiciona no playhead; arrastar no ruler move; Shift+↑ / Shift+↓ navega |
| Ir ao corte anterior/seguinte | ↑ / ↓ |
| Snapping | N liga e desliga |
| Clipe composto | Alt+C compõe a seleção; Alt+Shift+C decompõe; duplo clique abre a timeline dele |
| Desfazer / refazer | Ctrl+Z / Ctrl+Shift+Z ou Ctrl+Y |
| Zoom da timeline | Ctrl+roda, Ctrl+= / Ctrl+-; Shift+Z ajusta tudo |
| Exportar H.264/MP4 | Ctrl+E |
| Salvar / abrir projeto | Ctrl+S / Ctrl+O (`.veproj`, JSON) |

Uma faixa vermelha no topo de um clipe indica que imagem e som do mesmo take saíram de sincronia.
Isso só acontece quando uma trilha travada ou sem sync lock fica para trás.

## Ferramentas de teste

```powershell
./build/windows-release/ve-cli.exe encoders                       # quais H.264 existem e quais abrem
./build/windows-release/ve-cli.exe make-test-clip out.mp4 12 1920 1080 30
./build/windows-release/ve-cli.exe selftest out.mp4 result.mp4    # edita, salva, exporta e confere (fases 1 e 2)
./build/windows-release/ve-cli.exe play out.mp4 6                 # toca e mede o sincronismo A/V
./build/windows-release/ve-cli.exe render projeto.veproj saida.mp4 [encoder]
./build/windows-release/ve_core_tests.exe "[property]"           # só o teste de propriedade
./build/windows-release/ve_core_tests.exe "[golden]"             # só os estados dourados
```

Se uma mudança altera de propósito um estado dourado, rode os testes com `VE_UPDATE_GOLDEN=1`
para regravar `tests/core/golden/*.json` e revise o diff dos resumos antes de fazer commit.
`VE_PROPERTY_STATS=1` mostra quantas vezes cada edição foi aplicada no teste de propriedade.

O clipe de teste tem barras de cor, um marcador que anda com o número do frame e um flash
branco com bipe de 1 kHz no primeiro frame de cada segundo. Assim dá para checar o sincronismo
a olho e a ouvido.

Variáveis de ambiente:
- `VE_RHI=d3d11|d3d12|vulkan|opengl|metal`: backend do viewer. O padrão é D3D11 no Windows e
  Metal no macOS; o D3D12 também foi testado.
- `VE_FFMPEG_LOG=1`: logs detalhados do FFmpeg.
- `VE_TEST_MEDIA` e `VE_UI_SCREENSHOT`: entrada e screenshot dos testes de GUI (os das fases 2 e 3
  gravam `<nome>_phase2.png` e `<nome>_phase3.png`).
- O app também aceita `videoeditor arquivo.mp4 --seek 120 --play` e `--screenshot shot.png`.

O log fica em `%LOCALAPPDATA%\VideoEditor\VideoEditor\videoeditor.log` no Windows e em
`~/Library/Application Support/VideoEditor/VideoEditor/` no macOS. Os layouts ficam em
`layouts.ini`, na pasta de configuração do app (a mesma do log no Windows).

## Limitações conhecidas

- **NVENC:** os builds do FFmpeg 9.0 exigem driver NVIDIA ≥ 610, e esta máquina tem o 595.79.
  Por isso o "Automático" cai no Media Foundation, que usa o *NVIDIA H.264 Encoder MFT*, ou
  seja, a mesma GPU. Com o driver atualizado, a NVENC entra sozinha.
- A pré-visualização e a exportação convertem para 4:2:0. A exportação ainda passa pela CPU
  (swscale), e não pelo grafo da GPU, e não converte a matriz de cor (fontes BT.601 saem
  marcadas como BT.709).
- Uma mídia com fps diferente do da timeline é conformada pelo frame mais próximo, sem retime.
- Ainda não há crossfade de áudio nos cortes, então pode haver clique. O modelo tem transições, mas
  nenhuma edição as cria nem o render as desenha (fase 4).
- Ao editar durante a reprodução, a reprodução reinicia no ponto atual (há um pequeno soluço).
- Fase 2:
  - Clipes com velocidade alterada (fit to fill) ficam mudos até existir time-stretch de áudio
    (§6.5).
  - Um clipe composto pode ser estendido além do conteúdo da timeline filha: o trecho a mais fica
    vazio.
  - Ripple delete e ripple trims recusam (ou limitam) a edição quando uma trilha com sync lock tem
    mídia no trecho que sairia, como no Avid. Para ignorar uma trilha (música, por exemplo),
    desligue o sync lock dela.

- Fase 3:
  - Os atalhos ainda são fixos, e não há trim por teclado nem editor de trim (passo 3.6).
  - Com um painel flutuando em foco, os atalhos da janela principal não respondem: clique na
    janela principal antes.
  - Desfazer uma edição feita em outra timeline muda aquela timeline sem trocar de aba.
  - O viewer de fonte usa a grade de frames da timeline; uma mídia com outro fps é conformada pelo
    frame mais próximo, como na timeline.

## Próximos passos sugeridos

1. Passo 3.6: selecionar pontos de edição, nudge de 1 frame, trim por timecode digitado, trim
   dinâmico com JKL, editor de trim e mapa de atalhos personalizável.
2. Fase 4 da especificação: keyframes, transições renderizadas, transformação, títulos,
   velocidade com rampas e time-stretch de áudio. Isso tira o áudio mudo dos clipes com fit to fill.
3. Grafo de render na GPU, compartilhado entre viewer e exportação. A cor viria do protótipo
   `colornode`: ACEScct, LUT tetraédrica e scopes.
4. Texturas sem cópia: D3D11VA para D3D11, e VideoToolbox/IOSurface para Metal.
5. Thumbnails e waveforms, JKL com velocidades.
6. Projeto em SQLite (histórico e autosave) e importação/exportação OpenTimelineIO. O `.veproj`
   atual é JSON em flicks (formato 2, agora com marcadores) e não lê o formato 1, de frames, usado
   antes da reestruturação.
7. Cache de render e proxies; páginas carregadas sob demanda; tradução pt_BR (`tr()` já está
   em toda a UI).
