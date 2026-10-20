# Editor de vídeo nativo: especificação do caso de estudo

Objetivo: escrever do zero um editor de vídeo **nativo** (Windows e macOS) com ferramentas de edição do porte das
observadas no DaVinci Resolve, para validar se é possível construir ferramentas desse nível. **Sem nenhum recurso de
monetização** (sem licenças, contas, lojas ou telemetria). Nenhum código, shader, LUT, modelo ou asset da Blackmagic
é usado: só as ideias de arquitetura documentadas em `davinci-resolve-21.1.1-architecture-findings.md` e
`davinci-resolve-21.1.1-color-pipeline-findings.md`.

---

## 1. Critérios de sucesso do caso de estudo

O projeto está validado quando, numa máquina comum (por exemplo a RTX 3060 desta), o editor:

1. edita uma timeline multitrilha (≥ 4 vídeo e ≥ 8 áudio) com inserção, sobrescrita, ripple, roll, slip, slide,
   lâmina, cortes J/L, transições, clipes compostos, multicam e mudança de velocidade com rampas;
2. reproduz 1080p e 4K H.264/HEVC em tempo real, com áudio sincronizado (erro < 1 frame) e scrubbing preciso por frame;
3. faz grading por nós na GPU em float (primárias, curvas, qualificador, janelas com tracking, LUT) com scopes;
4. mixa áudio com barramentos, automação, EQ, compressor e normalização de loudness (EBU R128);
5. exporta (H.264/HEVC/ProRes ou DNxHR) com a mesma imagem do viewer;
6. tem desfazer/refazer de todas as operações, autosave e reabertura do projeto idêntico;
7. passa uma suíte automatizada de testes do núcleo de edição (estados de timeline "dourados").

---

## 2. Princípios de arquitetura (o que tornar o Resolve "profissional" ensina)

| Princípio | Evidência observada no Resolve | Como aplicar aqui |
|---|---|---|
| Núcleo de dados separado da interface | Modelo `Sm2*` independente das classes `Ui*` | Biblioteca `core` em C++ puro (sem Qt) com modelo, operações e desfazer; a UI só observa e envia comandos |
| Toda alteração é um comando com desfazer, agrupável em transação | Famílias `*Act*`; `FLActCreateTransaction`/`Commit`/`Abort` | Padrão Command + transações (um arraste = um passo de desfazer) |
| Motor de mídia e render desacoplado, por filas | `GsManager`/`LsManager`/`DpManager` com filas de mensagens; `GsTask*Planner` | Threads de decodificação, planejador de frames, cache e relógio de áudio, falando por filas sem lock |
| Processamento de imagem em float na GPU, num grafo | `ReactorGPU` + `DVIP::*Imp<API, float \| half>` | Um executor de grafo na GPU (RHI) com texturas RGBA16F/32F e shaders compilados sob demanda e guardados em cache |
| Caches em vários níveis | Render cache, cache de fluxo, cache por nó | Cache de frames decodificados (LRU), cache de saída por nó (hash dos parâmetros) e render cache em disco |
| Páginas e recursos pesados carregados sob demanda | `libFairlightPage`/`libFusionPage` via `dlopen` | Módulos por área (edit, cor, áudio), com recursos pesados (IA, OFX) carregados só quando usados |
| Plugins de terceiros isolados | `OFXLoader`, `VstScanner` | Varredura e hospedagem de plugins em processo separado (fase avançada) |
| Projeto em banco de dados | protobuf + SQLite/PostgreSQL | SQLite com tabelas normalizadas e blobs de parâmetros; autosave incremental |
| Padrões da indústria | ACES, OCIO, OFX, OTIO, AAF, EDL | OCIO/ACES na cor, OTIO/EDL/FCPXML no intercâmbio, OFX nos efeitos |

---

## 3. Pilha técnica (sem custo, adequada a um estudo não comercial)

| Área | Escolha | Licença |
|---|---|---|
| Linguagem/build | C++20, CMake, vcpkg | – |
| UI | Qt 6 (Widgets + viewer em QRhi) | LGPLv3 |
| GPU | QRhi: D3D12/D3D11 (Windows) e Metal (macOS); compute shaders em GLSL/HLSL compilados pelo `qsb` (QShaderBaker) | – |
| Mídia | FFmpeg (libavformat/avcodec/swresample) com decodificação acelerada: D3D11VA/NVDEC e VideoToolbox | LGPL (sem x264/x265 para continuar LGPL; para estudo, GPL também serve) |
| Áudio (dispositivo) | miniaudio (WASAPI, CoreAudio) ou RtAudio | MIT/MIT-like |
| Resampling | libsoxr | LGPL |
| Cor | OpenColorIO 2 + configurações ACES | BSD |
| Visão computacional | OpenCV (tracking, estabilização, fluxo óptico DIS) | Apache-2 |
| Intercâmbio | OpenTimelineIO | Apache-2 |
| Banco do projeto | SQLite | domínio público |
| IA (opcional) | whisper.cpp (transcrição), ONNX Runtime (DirectML/CoreML) | MIT |
| Testes | Catch2 ou GoogleTest | BSL/BSD |

---

## 4. Estrutura do código

```
videoeditor/
  core/        tempo racional, modelo (projeto, mídia, timeline, trilhas, itens), operações de edição,
               keyframes, comandos/desfazer, serialização (sem Qt; 100% testável)
  media/       probe, demux, decodificação (com aceleração de hardware), encode, proxies, waveform/thumbnail
  engine/      planejador de frames, cache LRU, relógio mestre (áudio), reprodução, render/export, render cache
  gpu/         abstração RHI, cache de shaders, executor do grafo de imagem, upload sem cópia
  color/       nós de cor (portar as ideias do ColorNode), OCIO/ACES, LUT 3D, scopes
  audio/       grafo do mixer em tempo real, barramentos, automação, efeitos, loudness, medidores
  fx/          transições, transformação, composição, títulos, keyers, ajuste de velocidade
  analysis/    detecção de cortes, sincronia por áudio, tracking, estabilização, transcrição
  ui/          páginas: Mídia, Edição, Cor, Áudio, Entrega; timeline, viewer, inspector
  app/         main, preferências, gerenciador de projetos
  tests/       testes do core (estados dourados), decodificação exata, sincronia A/V, render determinístico
```

Regra: `core` não depende de nada acima dele. `engine` depende de `core`/`media`/`gpu`. A UI fala com o `core`
apenas por comandos e observa eventos.

---

## 5. Fundamentos que tudo usa

### 5.1 Tempo racional (precisão de frame)
- Unidade interna em inteiro de 64 bits: **flicks** (1/705.600.000 s). Esse valor divide exatamente todas as cadências
  comuns (23,976 = 24000/1001, 24, 25, 29,97, 30, 50, 59,94, 60, 120 fps) e as taxas de áudio de 44,1/48/96 kHz.
- Nunca usar `double` para posições na timeline; converter para frame só na borda (UI e decodificação).
- `TimeRange{start, duration}` e helpers: `frameAt(time, rate)`, `timeOfFrame(n, rate)` e arredondamento definido
  (sempre para o frame cuja janela contém o instante).

### 5.2 Modelo de dados
```
Project ─ MediaPool ─ Folder* ─ MediaItem (arquivo, streams, timecode, taxa, metadados, proxy)
        └ Timeline* ─ VideoTrack*/AudioTrack* ─ Item*
Item = Clip(mediaRef, sourceRange, timelineStart, enabled, linkGroup, effects[], grade, retime)
     | Gap | Transition(in/out, alignment) | CompoundClip(timelineRef) | MulticamClip(angles[], activeAngle)
```
- Cada parâmetro animável é um `AnimatedParam` (valor fixo ou curva de keyframes).
- Clipes de áudio e vídeo vinculados compartilham um `linkGroup` (sincronia e indicadores de desvio).

### 5.3 Comandos, transações e desfazer
- `Command{do(), undo(), mergeWith()}`; uma `Transaction` agrupa comandos num único passo de desfazer.
- Arrastes (trim, sliders) abrem uma transação no `mousePress` e fazem commit no `mouseRelease`; os passos intermediários se fundem.
- Toda operação do core é validada antes (handles disponíveis, sobreposições) e é atômica: ou aplica tudo, ou nada.

---

## 6. Ferramentas de edição complexas: o que é e como funciona

> Cada item tem a **regra** (semântica) e o **teste de aceitação**. Todas vivem no `core` e são testadas sem UI.

### 6.1 Edição em 3 e 4 pontos
- **Inserir:** coloca o clipe no playhead (ou In da timeline), divide o que estiver ali e empurra para a direita
  tudo que vem depois, em todas as trilhas com *sync lock*.
- **Sobrescrever:** substitui o intervalo na trilha de destino sem deslocar nada.
- **Substituir:** troca o clipe sob o playhead mantendo a duração; alinha o frame da fonte ao playhead.
- **Ajustar ao intervalo (fit to fill):** encaixa o In/Out da fonte no In/Out da timeline mudando a velocidade.
- **Colocar por cima:** usa a primeira trilha livre acima.
- **Aceitação:** para cada operação, estados antes/depois comparados com JSON dourado, inclusive com trilhas travadas.

### 6.2 Trims (o coração de um editor profissional)
| Modo | O que muda | Restrição |
|---|---|---|
| Ripple | Out (ou In) de um clipe; tudo depois se desloca | Handles de mídia do clipe |
| Roll | Ponto de corte entre dois clipes; duração total igual | Handles do clipe que sai e do que entra |
| Slip | In/Out da fonte; posição e duração iguais | Handles nos dois lados |
| Slide | Posição do clipe entre vizinhos; vizinhos ajustam Out/In | Handles dos vizinhos |
- Trim assimétrico (vídeo e áudio separados) gera **cortes J e L**.
- **Lâmina (blade)** divide na posição; **ripple delete** remove e fecha o buraco nas trilhas com sync lock.
- **Snapping:** pontos de edição, marcadores, playhead e In/Out com tolerância em pixels convertida para tempo.
- **Aceitação:** propriedade "nenhum clipe usa mídia fora da fonte" e "nenhuma sobreposição na mesma trilha" após
  qualquer sequência aleatória de trims (teste de propriedade com 10 mil sequências).

### 6.3 Clipes compostos e timelines aninhadas
- A seleção vira um `CompoundClip` que referencia uma nova `Timeline`; o pai renderiza a filha como uma fonte.
- **Aceitação:** compor e decompor deve devolver o mesmo estado; o render da composta é idêntico (hash) ao original.

### 6.4 Multicam
- Ângulos sincronizados por **timecode**, **ponto de In** ou **áudio**.
- Sincronia por áudio: envelope em 1 kHz de cada ângulo, correlação cruzada via FFT (GCC-PHAT para robustez),
  pico → deslocamento em amostras → arredondar para frame.
- O corte multicam alterna o `activeAngle` em pontos de edição; o viewer mostra a grade de ângulos.
- **Aceitação:** clipes sintéticos com deslocamentos conhecidos (±3 s) sincronizados com erro ≤ 1 amostra a 48 kHz.

### 6.5 Velocidade, rampas e retime
- `TimeMap`: função tempo da timeline → tempo da fonte (constante, keyframes de velocidade ou curva de retime direta,
  o que permite congelar e reverter).
- Interpolação de frames: **mais próximo**, **mistura de frames** e **fluxo óptico** (fluxo DIS do OpenCV nos dois
  sentidos, warp dos dois frames até a fração `t` e mistura com máscara de oclusão).
- O áudio acompanha com time-stretch (WSOLA ou phase vocoder) e preserva o tom.
- **Aceitação:** rampa 100%→25%→100% preserva a duração calculada; com mistura de frames, o frame no meio é a média exata.

### 6.6 Keyframes e curvas
- Interpolações: linear, Bézier (Hermite cúbica com tangentes editáveis), constante (hold) e presets de ease.
- Editor de curvas por parâmetro; keyframes acompanham o clipe ao mover ou aparar.
- **Aceitação:** avaliação analítica comparada em 1 000 pontos; Bézier sem "overshoot" quando as tangentes estão travadas.

### 6.7 Transições, composição e transformação
- Transições exigem handles; alinhamento centralizado, no início ou no fim do corte. Tipos: dissolve, dip to
  color, wipes com suavidade e push.
- Transformação: posição, escala, rotação, âncora, crop, opacidade e modos de mesclagem (feitos em float linear).
- Títulos: texto renderizado para textura (QTextLayout/QPainter ou HarfBuzz + FreeType), com contorno e sombra.

### 6.8 Marcadores, metadados e busca
- Marcadores com cor, duração e notas; *smart bins* por regras (metadados, nome, tipo).

---

## 7. Motor de reprodução e render

### 7.1 Pipeline de um frame
```
para cada trilha de vídeo (de baixo para cima):
  item ativo no tempo t → TimeMap → frame(s) da fonte → decodificação (cache) → textura GPU
  → transformação de entrada (cor) → efeitos do clipe → grade do clipe (grafo de nós) → transformação/crop
  → composição sobre o acumulado (opacidade, modo)
→ efeitos da timeline → transformação de saída (DRT/ODT) → viewer, scopes e encoder
```

### 7.2 Reprodução em tempo real
- **Relógio mestre = áudio:** o vídeo apresenta o frame cujo PTS ≤ relógio do dispositivo; se atrasar, descarta;
  se adiantar, repete.
- **Decodificação antecipada:** um decodificador por clipe ativo num pool de threads. A busca vai ao keyframe anterior
  (GOP) e decodifica para a frente; fila de N frames à frente do playhead.
- **Scrubbing:** usa o proxy se existir. Em arraste rápido, só I-frames; ao parar, o frame exato.
- **Cache de frames LRU** com chave (mídia, frame, resolução, proxy); orçamento de RAM e de VRAM configurável.
- **Sem cópia:** frames de hardware (D3D11 / CVPixelBuffer + IOSurface) viram texturas sem passar pela CPU.

### 7.3 Proxies, mídia otimizada e render cache
- Gerar proxies em segundo plano (DNxHR LB / ProRes Proxy / H.264 intra) com o mesmo timecode; troca transparente.
- **Render cache:** salvar em disco a saída de nós caros (chave = hash dos parâmetros + hash da entrada) e invalidar
  pelo hash; renderizar em segundo plano quando ocioso.

### 7.4 Exportação
- O mesmo pipeline do viewer, em resolução total e sem descartes; fila de render com presets (H.264/HEVC por
  hardware, ProRes/DNxHR, sequência de imagens EXR/DPX/PNG).
- **Aceitação:** o frame exportado tem o mesmo hash do frame do viewer em resolução total.

---

## 8. Cor (a partir do ColorNode)
- Portar para compute shaders RHI: ACEScct como espaço de trabalho, grafo de nós (serial, paralelo, camada),
  primárias, curvas, curvas de matiz, qualificador HSL, janelas, LUT 3D tetraédrica e scopes por acumulação na GPU.
- Novidades em relação ao ColorNode: **tracking de janela** (§10), cache de saída por nó, OCIO para entrada/saída
  e precisão de 32 ou 16 bits selecionável.

---

## 9. Áudio (estilo Fairlight, versão estudo)
- **Thread de tempo real sem alocações nem locks:** comunicação por filas SPSC e snapshots atômicos de parâmetros.
- **Grafo:** fonte do clipe (resample com soxr) → ganho e fades do clipe → inserts da trilha (EQ biquad de 4 bandas,
  compressor, limiter) → pan → envios para barramentos → master → medidores.
- **Automação:** curvas amostradas por bloco com rampas lineares precisas por amostra (sem "zipper noise").
- **Loudness:** ITU-R BS.1770 (filtro K + gating) → LUFS integrado/curto/momentâneo; normalizar para −14/−16/−23 LUFS.
- **Formas de onda:** cache de picos min/máx em várias resoluções, calculado em segundo plano.
- **Aceitação:** seno de 1 kHz −20 dBFS medido com erro < 0,1 dB; latência de ida e volta medida; sem dropouts em 64 trilhas.

---

## 10. Análise (opcional, depois do núcleo)
- **Detecção de cortes:** diferença de histograma HSV (qui-quadrado) com limiar adaptativo e mínimo de frames.
- **Estabilização:** features (goodFeaturesToTrack) → fluxo LK piramidal → afim parcial com RANSAC por frame →
  trajetória acumulada → suavização (gaussiana ou L1) → correção + zoom para esconder bordas.
- **Tracking de janelas:** pontos (LK) para translação/escala/rotação; planar (homografia com RANSAC) para perspectiva.
- **Transcrição e legendas:** whisper.cpp → segmentos com tempo → legendas editáveis → exportar SRT/WebVTT.
- **Máscara de objeto (estilo Magic Mask):** modelo de segmentação aberto via ONNX Runtime, refinado por frame com tracking.

---

## 11. Persistência e intercâmbio
- SQLite: `project`, `media`, `timeline`, `track`, `item`, `keyframe`, `grade_node`, `marker`; parâmetros
  complexos em JSON/blob; autosave incremental a cada transação (WAL).
- Importar/exportar OpenTimelineIO (e daí EDL/FCPXML/AAF pelos adaptadores do OTIO).

---

## 12. Fases (cada uma termina com algo que roda)

| Fase | Entrega | Prova de que funciona |
|---|---|---|
| 0. Fundação | CMake, Qt 6, FFmpeg, RHI, CI local, testes do core | App abre; `ctest` passa |
| 1. Reproduzir e exportar | Importar, media pool, timeline V1/A1, reprodução sincronizada, exportar H.264 | Teste da claquete: A/V sincronizados; frame N decodificado exato |
| 2. Editor de verdade | Multitrilha, todas as edições do §6.1–6.3, desfazer/transações, snapping, marcadores | Estados dourados + teste de propriedade com 10 mil trims |
| 3. Movimento e tempo | Keyframes, transições, transformação, títulos, velocidade e rampas, mistura de frames | Hashes de render determinísticos |
| 4. Cor | Grafo de nós na GPU, scopes, LUT, qualificador, janelas | Igualdade numérica com o ColorNode nos mesmos parâmetros |
| 5. Áudio | Mixer, barramentos, automação, EQ/compressor, loudness, formas de onda | Medições do §9 |
| 6. Desempenho | Proxies, cache de frames, render cache, decodificação por hardware, fila de render | 4K HEVC em tempo real; scrubbing fluido |
| 7. Avançado | Multicam com sincronia por áudio, fluxo óptico, estabilização, tracking, transcrição, OTIO | Testes sintéticos do §6.4, §6.5 e §10 |

---

## 13. Riscos e como reduzir

| Risco | Mitigação |
|---|---|
| Sincronia A/V e busca precisa em codecs long-GOP | Testes de frame exato desde a fase 1; proxies intra-frame |
| Desempenho da GPU entre D3D e Metal | QRhi + compute shaders; medir desde a fase 1 (orçamento de ms por frame) |
| Complexidade das regras de trim | Core puro com testes de propriedade antes da UI |
| Áudio em tempo real | Thread isolada sem alocação; medir dropouts automaticamente |
| Escopo enorme | Fases com entregas executáveis; nada de IA antes da fase 7 |

---

## 14. Mapa: o que foi observado no Resolve → o que fazemos aqui

| Observado (nome interno) | Equivalente no caso de estudo |
|---|---|
| `Sm2Timeline`, `Sm2TiTrack`, `Sm2TiVideoClip`, `Sm2TimeMap` | `core::Timeline`, `Track`, `Clip`, `TimeMap` |
| `Sm2MpCompoundClip`, `Sm2MpMulticamClip`, `Sm2MpFusionClip` | `CompoundClip`, `MulticamClip` (sem Fusion) |
| `*Act*` + `FLActCreateTransaction/Commit/Abort` | `Command` + `Transaction` |
| `GsManager`/`GsTask*Planner`, filas High/Low | `engine::Planner` + filas de prioridade |
| `ReactorGPU` + `DVIP::*Imp<API, float \| half>` | `gpu::GraphExecutor` + shaders com variantes |
| `Sm2RenderCacheMgr`, `FlowCacheManager` | `engine::RenderCache`, cache de fluxo óptico |
| `IO*` (119 codecs) | `media::` via FFmpeg + aceleração de hardware |
| `faa::`, `VPlayer`/`VTrack`, `SRC::Ext::Soxr`, ADM | `audio::Mixer`, soxr; ADM/Atmos fora do escopo |
| `DVIP::Tracker`, `OpticalFlow`, `VideoStabilizer` | `analysis::` com OpenCV |
| `DVIP::SpeechRecognition` (Kaldi) | whisper.cpp |
| SQLite/PostgreSQL com blobs protobuf | SQLite com JSON/blobs |
| `opentimelineio` | OpenTimelineIO |
