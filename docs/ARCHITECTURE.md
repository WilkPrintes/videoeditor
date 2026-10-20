# Arquitetura

## Camadas (spec §4)

```
 app/ (videoeditor)          tools/ve-cli
        │                         │
 ui/ (Qt Widgets + QRhi viewer)   │        tests/ui (Qt Test)
        │ Commands / gestos       │
 engine/ (reprodução, export) ◄───┘
        │
 audio/ (miniaudio, mixer)
        │
 media/ (FFmpeg: probe, decoders, MP4 writer)
        │
 core/ (tempo, modelo, edições, transações, JSON)   ◄── tests/core (Catch2)
```

- **core** não depende de Qt nem de FFmpeg. É o modelo, as regras de edição, o desfazer e a
  serialização, 100% testável isoladamente.
- **media/audio/engine** não dependem de Qt. São o I/O de mídia e o tempo real (reprodução e
  exportação), reaproveitáveis num render headless ou num processo separado.
- **ui** só lê o modelo e emite *pedidos* (mover, trim, cortar...). A `MainWindow` transforma
  cada pedido em Command. Nenhum widget altera o modelo diretamente.
- `color/`, `fx/` e `analysis/` estão reservadas para as fases 3, 4 e 7.

## Tempo (spec §5.1)

- Toda posição e duração é um inteiro em **flicks** (`ve::Time`, 1/705 600 000 s). O flick divide
  exatamente a duração de frame de 23,976/24/25/29,97/30/48/50/59,94/60/120 fps e a de amostra de
  44,1/48/88,2/96/192 kHz, então nada acumula erro.
- Frames e segundos só existem nas bordas: `frameAt`, `timeOfFrame`, `sampleAt` e
  `timeOfSample`. Todas usam aritmética inteira exata (`mulDivFloor`), e a regra de janela é
  sempre a mesma: um instante pertence ao frame cuja janela o contém.
- As edições mantêm os clipes na grade de frames da timeline. A mídia com outra cadência é
  conformada pelo frame mais próximo na decodificação.

## Modelo de dados (spec §5.2, inspirado no OpenTimelineIO)

```
Project ─ MediaPool ─ Folder* ─ MediaItem (arquivo, streams, timecode, taxa, metadados, proxy)
        └ Timeline* ─ Track* (Video | Audio, lock, sync lock) ─ Item*
Item = Clip(mediaId, sourceRange, enabled, linkGroup, effects[], grade, retime)
     | Gap | Transition(in/out, alinhamento) | CompoundClip(timelineId) | MulticamClip(angles[], activeAngle)
```

- Como no OTIO, a trilha é uma **sequência**: o início de um item na timeline é a soma das durações
  anteriores, e o espaço vazio é um `Gap` explícito. Assim, sobreposição na mesma trilha é
  impossível por construção. Transições não ocupam tempo.
- `AnimatedParam` é um valor fixo ou uma curva de keyframes (linear e hold; Bézier na fase 3).
- **Clipes ligados:** vídeo e áudio importados juntos compartilham um `linkGroup` e editam juntos.
  Quando uma edição parte clipes em várias trilhas ao mesmo tempo, `IdRemap` dá às metades novas
  um grupo novo, igual entre as trilhas.
- As primitivas da trilha (`liftRange`, `placeItem` com overwrite, `splitAt`, `normalize`) ficam
  em `Track`. As operações de usuário (`placeMedia`, `splitAt`, `moveClips`, `trimClip`,
  `deleteClips`) ficam em `core/Edit.h` e respeitam trilhas travadas.
- `edit::checkInvariants` define o que é uma timeline válida: grade de frames, handles de mídia e
  nenhum gap sobrando. O teste de propriedade aplica isso a cada edição.

## Comandos, transações e desfazer (spec §5.3)

No Resolve, as edições aparecem como famílias `*Act` com `CreateTransaction/Commit/Abort`. Aqui:

- `Command{redo, undo, mergeWith}` empilhados no `CommandStack`, que guarda o índice "limpo"
  (salvo) e notifica a UI.
- `TimelineEditCommand` executa a operação **uma vez** e guarda os snapshots antes e depois da
  timeline. Undo e redo só trocam snapshots, então nenhuma edição precisa de lógica inversa. Se
  a operação recusa no meio, o snapshot é restaurado: cada operação é **atômica**.
- **Transações:** `beginTransaction` / `commit` / `abort`. Tudo o que é empurrado entre elas
  vira um único passo de desfazer, e `abort` volta ao estado anterior.
- **Gestos:** um arraste na timeline abre uma transação no press e empurra um comando por
  movimento do mouse, com a mesma `mergeKey`. Comandos com a mesma chave **se fundem**: o anterior
  é desfeito e o novo é aplicado a partir do estado do início do gesto. A timeline e o viewer
  acompanham o mouse ao vivo, e passar por cima de um clipe e voltar não o destrói. O release
  faz commit; Esc faz abort.
- Comandos que não mudam nada (corte onde já há corte, move de 0 frame) são descartados.

## Persistência

`core/ProjectFile` gera JSON no estilo OTIO (objetos com `schema`, trilhas com `children`), com
tempos em flicks (formato 2). Os caminhos da mídia são guardados absolutos e relativos ao projeto,
para religar se a pasta for movida. O JSON é próprio do core (`core/Json`): sem dependências,
independente de locale e com int64 exato. A gravação é atômica (arquivo temporário + rename).
Um banco SQLite com autosave incremental fica para a fase de persistência (spec §11).

## Motor de reprodução

```
 UI (thread principal, timer de 8 ms)        thread de vídeo                 thread de áudio
 ─────────────────────────────────────       ─────────────────────────────   ─────────────────────────
 setTimeline(snapshot) ───────────────────►  decodifica frames à frente      mixa A1..An em blocos de
 play / pause / seek                         numa fila (8 frames), ou          1024 amostras → ring SPSC
 currentVideoFrame()  ◄── fila / still       atende o último pedido de        (lock-free)
 position()  ◄── relógio                     scrub (o mais recente vence)              │
                                                                             callback do dispositivo
                                                                             (WASAPI / Core Audio)
                                                                             consome o ring:
                                                                             amostras tocadas = RELÓGIO
```

- **O áudio é o relógio mestre.** A posição é `início + (amostras_tocadas − latência) / taxa`.
  Só amostras reais contam: num underrun o relógio para em vez de dessincronizar. Sem dispositivo
  de áudio, o relógio usa `steady_clock`.
- O vídeo é **apresentado pelo relógio**. A cada tick, a UI pega o frame mais recente com
  `frame ≤ relógio` e descarta os atrasados. Se o decoder ficar para trás, a fila pula adiante.
- **Scrubbing com precisão de frame:** `VideoDecoder::frameAt(t)` busca o keyframe anterior e
  decodifica até o frame mais próximo de `t`. Pedidos sequenciais não fazem seek. Pedidos de
  scrub se juntam, e só o mais recente é decodificado.
- **Hardware:** D3D11VA no Windows e VideoToolbox no macOS. O frame é baixado para 4:2:0
  planar, 8 ou 16 bits. Se o hardware não suportar o perfil, cai para software.
- **Snapshots imutáveis:** a UI cria um `TimelineSnapshot` (timeline + mídias) a cada mudança.
  As threads do motor nunca leem o modelo vivo.

## Viewer na GPU (Qt RHI)

```
 planos Y/U/V (R8 ou R16) ──► passo 1 "process" ──► RGBA16F (float) ──► passo 2 "display" ──► widget
                              YUV→RGB (matriz e faixa                    ajuste de aspecto
                              em uniforms)
```

- `QRhiWidget` usa D3D11 por padrão no Windows (D3D12 com `VE_RHI=d3d12`) e Metal no macOS. Os
  shaders são GLSL 440, compilados pelo `qsb` para HLSL, MSL e SPIR-V no build.
- O alvo RGBA16F intermediário é onde o grafo de efeitos e cor vai entrar (ACEScct, LUTs,
  nós), já em float, como no Resolve.

## Exportação

`exportTimeline()` percorre a timeline frame a frame:
- **Vídeo:** pega o clipe de cima (ou preto), redimensiona com letterbox via swscale e passa ao
  encoder H.264.
- **Áudio:** passa pelo mesmo `AudioMixer` da reprodução, com amostras exatas por frame, e vai
  para o AAC.
- **Saída:** MP4 com `+faststart`.

Ordem dos encoders no modo automático:
- **Windows:** NVENC → AMF → Quick Sync → x264 (só em build GPL) → Media Foundation (MFT de
  hardware primeiro) → OpenH264.
- **macOS:** VideoToolbox → x264 → OpenH264.

Encoders do Media Foundation exigem COM MTA, então todo encode roda numa thread própria.

## Paralelos com o Resolve (análise estática) e o que ficou para depois

O mapa completo está no §14 da [especificação](ESPECIFICACAO.md).

| Resolve | Aqui |
|---|---|
| UI em Qt Widgets + toolkit desenhado na GPU | Qt Widgets + `QRhiWidget` |
| Páginas como bibliotecas carregadas sob demanda | ainda não: janela única (próximo passo) |
| Modelo Sm2Timeline/Track/Clip/Gap/Transition/Compound/Multicam em protobuf num banco SQLite/PostgreSQL | modelo OTIO-like (com compostos e multicam) em JSON com flicks; SQLite é o próximo passo |
| Ações *Act com undo e CreateTransaction/Commit/Abort | `Command` + `CommandStack` com transações e fusão de gestos |
| Gerentes com filas de mensagens e planejadores de tarefas | threads de vídeo e áudio com filas; snapshot por mudança |
| Grafo de nós na GPU em float, kernels compilados em runtime | passo float RGBA16F pronto; o grafo vem depois |
| FFmpeg + SDKs de câmera | FFmpeg (LGPL); o host de plug-ins de codec fica para depois |
| Host OFX, scripting Python/Lua | fora do marco 1 |
