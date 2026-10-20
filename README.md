# VideoEditor

Editor de vídeo não linear **nativo** para Windows e macOS, em C++20 com Qt 6 (Widgets e viewer
desenhado na GPU via Qt RHI), FFmpeg (LGPL, linkagem dinâmica) e miniaudio. "VideoEditor" é um
nome provisório.

É um **caso de estudo** para validar se é possível construir ferramentas de edição do porte das
de um editor profissional. Não tem nenhum recurso de monetização: nada de licença, conta, loja ou
telemetria. A especificação completa, com as fases 0–7 e os testes de aceitação, está em
[docs/ESPECIFICACAO.md](docs/ESPECIFICACAO.md).

A arquitetura segue padrões observados no DaVinci Resolve (análise estática): modelo de dados
separado da UI, edições como *Commands* com desfazer, motor de reprodução em threads próprias com
filas e processamento de imagem em float na GPU. Nenhum código do Resolve foi usado. Os detalhes
estão em [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

## Estado: fase 1 da especificação (marco 1)

| Critério | Situação no Windows 11 (RTX 3060) |
|---|---|
| Compila e abre | ✅ MSVC 14.51 (VS Build Tools 2026), Qt 6.12.0, FFmpeg 9.0 LGPL |
| Importa vídeo | ✅ menu, Ctrl+I ou arrastar arquivos; o primeiro vídeo define fps e resolução da timeline |
| Timeline V1/A1 com cortar e mover | ✅ blade (Ctrl+B), mover por arraste (com snapping), trim pelas bordas, apagar, desfazer/refazer; cada arraste é uma transação editada ao vivo (Esc cancela) |
| Reprodução com áudio sincronizado | ✅ o áudio é o relógio mestre; decodificação D3D11VA; desvio imagem/relógio de 0 frame medido |
| Exporta H.264/MP4 | ✅ H.264 + AAC; frames e áudio conferidos frame a frame contra a timeline |

Verificado por:
- `ve_core_tests` (Catch2, 33 casos e cerca de 20 mil asserções), que cobre:
  - tempo em flicks, JSON e arquivo de projeto;
  - edições e transações;
  - um **teste de propriedade com 10 000 edições aleatórias**. Ele confere grade de frames,
    handles de mídia, alinhamento vídeo/áudio e se o undo total volta ao estado inicial.
- `ve_ui_tests` (Qt Test): dirige a janela real com mouse e teclado (importar → cortar → arrastar
  ao vivo → trim → arraste cancelado com Esc → desfazer → tocar).
- `ve-cli selftest` e `ve-cli play`: veja [Ferramentas de teste](#ferramentas-de-teste). O export
  foi conferido frame a frame (luma dos flashes) e pelos bipes do áudio.

Já previsto na base para as próximas fases (§4–5 da especificação):
- **Tempo em flicks** (1/705 600 000 s): divide exatamente todas as cadências comuns e as taxas
  de áudio.
- **Modelo do §5.2**: pastas no media pool, parâmetros animáveis, efeitos, grade, retime, clipes
  compostos e multicam, trilhas com trava e sync lock.
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
ui/           janela principal, viewer QRhiWidget, timeline, media pool, export
app/          main.cpp
tools/ve-cli/ probe, export, auto-teste e teste de reprodução sem UI
tests/core/   Catch2: tempo, JSON, edições, comandos, projeto, propriedade
tests/ui/     Qt Test: teste de GUI
color/ fx/ analysis/   reservadas para as fases 3, 4 e 7
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
| Colocar mídia do pool | duplo clique (no fim) ou arrastar para a timeline (sobrescreve na posição) |
| Tocar/pausar | Espaço; L toca, K pausa |
| Frame anterior/próximo | ← / →; com Shift, ± 1 segundo |
| Início / fim | Home / End |
| Cortar no playhead (blade) | Ctrl+B ou B (com clipes selecionados, corta só eles) |
| Mover clipe | arrastar o corpo do clipe (vídeo e áudio ligados andam juntos; há snapping) |
| Trim | arrastar a borda esquerda ou direita do clipe |
| Apagar seleção | Delete / Backspace (deixa um gap) |
| Desfazer / refazer | Ctrl+Z / Ctrl+Shift+Z ou Ctrl+Y |
| Zoom da timeline | Ctrl+roda, Ctrl+= / Ctrl+-; Shift+Z ajusta tudo |
| Exportar H.264/MP4 | Ctrl+E |
| Salvar / abrir projeto | Ctrl+S / Ctrl+O (`.veproj`, JSON) |

## Ferramentas de teste

```powershell
./build/windows-release/ve-cli.exe encoders                       # quais H.264 existem e quais abrem
./build/windows-release/ve-cli.exe make-test-clip out.mp4 12 1920 1080 30
./build/windows-release/ve-cli.exe selftest out.mp4 result.mp4    # edita, salva, exporta e confere
./build/windows-release/ve-cli.exe play out.mp4 6                 # toca e mede o sincronismo A/V
./build/windows-release/ve-cli.exe render projeto.veproj saida.mp4 [encoder]
./build/windows-release/ve_core_tests.exe "[property]"           # só o teste de propriedade
```

O clipe de teste tem barras de cor, um marcador que anda com o número do frame e um flash
branco com bipe de 1 kHz no primeiro frame de cada segundo. Assim dá para checar o sincronismo
a olho e a ouvido.

Variáveis de ambiente:
- `VE_RHI=d3d11|d3d12|vulkan|opengl|metal`: backend do viewer. O padrão é D3D11 no Windows e
  Metal no macOS; o D3D12 também foi testado.
- `VE_FFMPEG_LOG=1`: logs detalhados do FFmpeg.
- `VE_TEST_MEDIA` e `VE_UI_SCREENSHOT`: entrada e screenshot do teste de GUI.
- O app também aceita `videoeditor arquivo.mp4 --seek 120 --play` e `--screenshot shot.png`.

O log fica em `%LOCALAPPDATA%\VideoEditor\VideoEditor\videoeditor.log` no Windows e em
`~/Library/Application Support/VideoEditor/VideoEditor/` no macOS.

## Limitações conhecidas

- **NVENC:** os builds do FFmpeg 9.0 exigem driver NVIDIA ≥ 610, e esta máquina tem o 595.79.
  Por isso o "Automático" cai no Media Foundation, que usa o *NVIDIA H.264 Encoder MFT*, ou
  seja, a mesma GPU. Com o driver atualizado, a NVENC entra sozinha.
- A pré-visualização e a exportação convertem para 4:2:0. A exportação ainda passa pela CPU
  (swscale), e não pelo grafo da GPU, e não converte a matriz de cor (fontes BT.601 saem
  marcadas como BT.709).
- Uma mídia com fps diferente do da timeline é conformada pelo frame mais próximo, sem retime.
- Ainda não há crossfade de áudio nos cortes, então pode haver clique. Há só uma timeline, com
  V1 e A1 na UI, embora o modelo já suporte N trilhas e transições.
- Ao editar durante a reprodução, a reprodução reinicia no ponto atual (há um pequeno soluço).

## Próximos passos sugeridos

1. Fase 2 da especificação: multitrilha e as edições do §6.1–6.3 (inserir, ripple, roll, slip,
   slide, J/L, compostos), com estados dourados em JSON. O modelo, as transações e o teste de
   propriedade já estão prontos para isso.
2. Grafo de render na GPU, compartilhado entre viewer e exportação. A cor viria do protótipo
   `colornode`: ACEScct, LUT tetraédrica e scopes.
3. Texturas sem cópia: D3D11VA para D3D11, e VideoToolbox/IOSurface para Metal.
4. Thumbnails e waveforms, JKL com velocidades, edições ripple/roll/slip/slide, mais trilhas,
   transições renderizadas.
5. Projeto em SQLite (histórico e autosave) e importação/exportação OpenTimelineIO. O `.veproj`
   atual é JSON em flicks (formato 2) e não lê o formato 1, de frames, usado antes da reestruturação.
6. Cache de render e proxies; páginas carregadas sob demanda; tradução pt_BR (`tr()` já está
   em toda a UI).
