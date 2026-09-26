# noshare-cover

[English](README.md) · [Архитектура и статус](ARCHITECTURE.md)

Плагин Hyprland. Окна с `no_screen_share` в захвате экрана закрываются картинкой или видео
вместо чёрного прямоугольника Hyprland. На самом экране окно остаётся как есть.

Версия 2.0 переписана: ядро на Rust (медиа, декод, часы воспроизведения, жизненный цикл,
публичный API), тонкая C++-прослойка общается с C++ API плагинов Hyprland. Без FFmpeg,
без cairo, без внешних процессов.

> **Статус.** Работает целиком на живом Hyprland (0.56.2, Arch, llvmpipe): окно с
> `no_screen_share` в захвате `grim` закрыто обложкой (картинка, GIF, H.264, AV1, VP9),
> многократные `hyprctl plugin unload/load` не дают роста потоков и памяти
> (`tests/e2e/run.sh`). Собирается Nix-ом против Hyprland 0.56.0 и 0.56.2 из nixpkgs и
> makepkg на Arch. GPU-декод (NVDEC, VA-API) написан, но на настоящих видеокартах ещё не
> запускался, см. [Проверка GPU](#проверка-gpu).

## Декод видео

| | Кодеки | Откуда |
|---|---|---|
| NVDEC (NVIDIA) | H.264, HEVC, VP8, VP9, AV1 | `libcuda`/`libnvcuvid` из драйвера, dlopen |
| VA-API (Intel, AMD) | H.264, HEVC, VP8, VP9, AV1 (8 бит) | вшитый помощник на cros-codecs, нужен `libva` |
| CPU | AV1 | rav1d, вшит |
| CPU | H.264 | системный `openh264`, dlopen |
| CPU | VP8, VP9 | системный `libvpx`, dlopen |

`backend = "auto"` пробует видеокарту из `gpu_device` (или первый render node) и при неудаче
переходит на CPU; `"gpu"` на CPU не переходит никогда; `"cpu"` видеокарту не трогает. Нет
нужной библиотеки — это не ошибка плагина: он один раз скажет, какой пакет поставить, и
возьмёт то, что есть.

Пока транслируется монитор с видео- или GIF-обложкой, плагин сам помечает её область
изменённой примерно 60 раз в секунду: Hyprland выдаёт кадры захвата только при изменениях на
мониторе, и без этого на статичном мониторе обложка в трансляции стояла бы.

## Конфиг

```lua
hl.config({
    plugin = {
        no_screen_share_cover = {
            -- png, jpg, jpeg, gif, mp4, m4v, mov, webm, mkv
            path_cover = "~/.config/hypr/noshare-cover.gif",
            loop = true,
            speed = 1.0,
            -- декод видео: "auto" (GPU, если можно, иначе CPU), "gpu" (только GPU), "cpu"
            backend = "auto",
            -- render node для GPU; пусто = первый /dev/dri/renderD*
            gpu_device = "",
            -- сколько мс держать обложку после конца анимации закрытия окна
            close_hold = 0,
            -- каким программам захвата (имя exe) показывать скрытые окна как есть
            show_to = "",
            -- если задано, прятать только от этих программ, остальные видят всё
            hide_from = "",
        },
    },
})
```

`path_cover` — обложка по умолчанию. Если он не задан, берётся первый существующий файл из
`~/.config/hypr/noshare-cover.{gif,jpg,jpeg,png,mp4}`; если нет ни одного, окна без своей
обложки просто получают чёрный бокс, без ошибки. `~` раскрывается.

Окну можно задать своё медиа, скорость и петлю правилом. Поля плагина плоские, обычные
Lua-имена, `hl.window_rule` принимает их напрямую, без обёрток:

```lua
hl.window_rule({
    match = { class = [[^(com\.ayugram\.desktop)$]] },
    no_screen_share = true,
    no_screen_share_cover = "~/.config/hypr/NoCover/67.mp4", -- медиа для этого окна
    no_screen_share_cover_speed = 1.5,                        -- необязательно
    no_screen_share_cover_loop = false,                       -- необязательно
    no_screen_share_cover_hold = 300,                         -- необязательно, вместо close_hold
})
```

Слои layer-shell (бары, лаунчеры, обои) так же, через layer rule:

```lua
hl.layer_rule({
    match = { namespace = "waybar" },
    no_screen_share = true,
    no_screen_share_cover = "~/.config/hypr/NoCover/bar.png",
})
```

Имена полей из исходного плагина (`["no_screen_share_cover:path_cover"]`, `[":speed"]`,
`[":loop"]`) тоже работают.

Без `no_screen_share` окно не закрывается. Если совпало несколько правил, побеждает последнее.

Окно, лежащее поверх скрытого, остаётся видно в стриме: обложка (или чёрный бокс) рисуется только
там, где скрытое окно реально видно. Под полупрозрачным окном сверху обложка тоже рисуется, а само
окно рисуется заново поверх неё, со своей прозрачностью и блюром, так что сквозь него видна
обложка, а не скрытое окно. Слои над окнами (бары, лаунчеры, уведомления) так же рисуются поверх
обложки.

При закрытии окна или слоя Hyprland подменяет его снимком для анимации закрытия, а на этот
снимок `no_screen_share` не действует, так что без плагина содержимое мелькает в стриме. Обложка
идёт за снимком до конца анимации и потом держится ещё `close_hold` мс (в правиле окна или слоя:
`no_screen_share_cover_hold`). `0` закрывает только саму анимацию.

Та же задержка работает, когда окно перестало быть скрытым, но осталось на экране, например
правило по заголовку перестало совпадать. Браузер сначала меняет заголовок и только потом
перерисовывается, поэтому при уходе с подходящей вкладки старая страница иначе попадает в стрим
на кадр-другой; для такого правила поставь `no_screen_share_cover_hold` (например, 300).
Программам из `show_to` задержка не применяется.

По умолчанию скрытые окна закрыты в любом захвате: и в стримах через портал (браузеры,
Discord, OBS через PipeWire), и у программ, которые снимают экран напрямую (grim,
wf-recorder, gpu-screen-recorder, OBS с wlrobs). Программа захвата определяется по её
исполняемому файлу (`/proc/<pid>/exe` Wayland-клиента). `show_to = "grim, hyprshot"` показывает
этим программам скрытые окна как есть, например для своих скриншотов. `hide_from =
"xdg-desktop-portal-hyprland"` наоборот: окна прячутся только от перечисленных (здесь только
от стримов через портал), а клиент, которого не удалось определить, всё равно видит обложку.
Имена через запятую или пробел. Если оба списка пустые, плагин клиента вообще не смотрит.

Те же списки можно задать в правиле окна или слоя, тогда для этого окна действуют только они:

```lua
hl.window_rule({ match = { class = "org.telegram.desktop" }, no_screen_share = true,
    no_screen_share_show_to = "grim" })               -- на скринах видно, в стриме нет
hl.layer_rule({ match = { namespace = "waybar" }, no_screen_share = true,
    no_screen_share_hide_from = "xdg-desktop-portal-hyprland" })
```

Скрытый фоновый слой (например, обои) закрывается обложкой целиком, вместе с окнами поверх
него, так же как чёрный бокс самого Hyprland.

Зум курсора (`cursor:zoom_factor`) тоже учтён: в стрим идёт уже увеличенная картинка, а свои
чёрные боксы `no_screen_share` Hyprland ставит так, будто зума нет, и они мимо окон. Пока
монитор под зумом, плагин рисует все боксы сам там, где окна реально находятся: обложки или
обычный чёрный для окон (и их попапов) без обложки.

Ошибки (нет файла, неизвестный формат, битое видео, неверный `backend`) показываются
уведомлением Hyprland один раз, а не каждый кадр. Пропавший файл подхватывается сам, как
только появится.

Если плагин загружен через hyprpm, при старте Hyprland может показать `unknown config key
'plugin.no_screen_share_cover...'`: конфиг читается раньше, чем hyprpm грузит плагин. После
загрузки плагина Hyprland перечитывает конфиг, и ошибка уходит.

## API для других плагинов

Оверлеи с превью окон (например, gloview) могут попросить noshare-cover закрыть и их
прямоугольники: чёрным или той же обложкой, что у окна. Только заголовок, линковать ничего не
надо: [`include/noshare_cover_api.h`](include/noshare_cover_api.h).

```c
#include "noshare_cover_api.h"

static noshare_cover_api nsc;
static uint64_t          client;

// PLUGIN_INIT (или позже, когда noshare-cover появился)
if (noshare_cover_bind(&nsc) == 0) {
    client = nsc.register_client("my-overlay");
    // необязательно: узнать о выгрузке noshare-cover и не держать его в памяти
    if (nsc.set_gone_callback && nsc.set_gone_callback(client, on_gone, NULL))
        noshare_cover_drop_handle(&nsc);
}

// каждый кадр оверлея, на монитор: атомарно заменяет прямоугольники этого клиента на нём
noshare_cover_rect r = {x, y, w, h, rounding, window_address, NOSHARE_COVER_FILL_WINDOW};
nsc.set_rects(client, monitor_id, &r, 1);

// оверлей закрылся:  nsc.set_rects(client, monitor_id, NULL, 0);
// PLUGIN_EXIT:       nsc.unregister_client(client); noshare_cover_unbind(&nsc);
```

Координаты — глобальные пиксели раскладки (то же пространство, что позиция и размер окна).
У каждого клиента свои прямоугольники, один плагин не сотрёт чужие. `on_gone` вызывается
из `PLUGIN_EXIT` noshare-cover уже после снятия его хука `renderMonitor`: забудьте все
указатели на его функции. Функции v1 `noshare_cover_clear_extra_rects` /
`noshare_cover_add_extra_rect` работают как раньше.

Если `renderMonitor` уже перехвачен другим плагином (gloview делает так, пока noshare-cover
не загружен), noshare-cover не отказывается от загрузки, а ждёт, пока тот отпустит функцию.
gloview отпускает её сам, как только видит noshare-cover, так что порядок загрузки неважен.

## Зависимости

**Для сборки**

| Что | Зачем | Arch | Nix |
|---|---|---|---|
| `cargo` / `rustc` (1.89+) | ядро на Rust | `rust` | есть во флейке |
| компилятор C++ (C++26), `make`, `pkg-config` | прослойка для Hyprland | `base-devel`, `pkgconf` | есть во флейке |
| заголовки Hyprland | API плагинов | `hyprland` (hyprpm ставит свои) | из пакета Hyprland |
| `nasm` | ассемблер для декодера AV1 (rav1d) | `nasm` | есть во флейке |
| `clang` / libclang | привязки для помощника VA-API | `clang` | `bindgenHook` |
| заголовки `libva`, `gbm` | помощник VA-API | `libva`, `mesa` | есть во флейке |

`make NSC_VAAPI=0` собирает без VA-API; тогда clang, libva и gbm не нужны. `make` сначала
всё проверяет и пишет, чего не хватает.

**Для работы** (всё необязательно, плагин грузится и без этого)

| Что | Для чего | Arch |
|---|---|---|
| драйвер NVIDIA (`libcuda`, `libnvcuvid`) | декод на видеокарте NVIDIA (NVDEC) | `nvidia-utils` |
| `libva` + драйвер VA-API | декод на видеокарте Intel/AMD | `libva` + `mesa` / `intel-media-driver` |
| `openh264` | H.264 на процессоре | `openh264` |
| `libvpx` | VP8/VP9 на процессоре | `libvpx` |

AV1 на процессоре, картинки и GIF ничего дополнительно не требуют. В Nix `openh264` и
`libvpx` берутся из store, драйверы NVIDIA и VA-API — из системы (`/run/opengl-driver`).

## Установка

### hyprpm (Arch и другие)

Для сборки нужны `cargo`, `nasm`, `clang` и заголовки `libva` (Arch:
`pacman -S rust pkgconf nasm clang libva`). Если чего-то нет, `make` сразу напишет, чего
именно и какой пакет поставить. Без VA-API: `make NSC_VAAPI=0`.

```sh
hyprpm add https://github.com/gitscout-bot/noshare-cover
hyprpm enable noshare-cover
hyprpm reload
```

URL без `/` в конце: со слэшем hyprpm не может взять имя репозитория и ставит плагин мимо
своей папки. hyprpm собирает плагин против запущенного Hyprland и грузит его сам, отдельно
`hl.plugin.load` не нужен.

### Arch

`packaging/arch/PKGBUILD` (`makepkg -si`). Пересобирайте после каждого обновления
`hyprland`: плагин, собранный под другие заголовки, откажется грузиться, а не уронит
композитор.

```lua
hl.plugin.load("/usr/lib/hyprland/plugins/libnoshare-cover.so")
```

### Nix

Плагин нужно собирать ровно против того Hyprland, который запущен, иначе он не загрузится с
ошибкой `built for Hyprland <коммит>, running <коммит>`. Проще всего через модуль Home Manager:
он собирает против системного `programs.hyprland.package`, если включён модуль NixOS (это тот
Hyprland, что запускается через `/run/wrappers`), иначе против
`wayland.windowManager.hyprland.package`:

```nix
inputs.noshare-cover = {
  url = "github:gitscout-bot/noshare-cover";
  inputs.nixpkgs.follows = "nixpkgs";
};
```

```nix
imports = [ inputs.noshare-cover.homeManagerModules.default ];
programs.noshare-cover.enable = true;
# добавляет его в wayland.windowManager.hyprland.plugins; грузить самому:
# "${config.programs.noshare-cover.package}/lib/libnoshare-cover.so"
```

Без Home Manager, на NixOS:
`inputs.noshare-cover.lib.mkNoshareCover pkgs config.programs.hyprland.package`.

Готовые пакеты подходят только под конкретные сетапы. `packages.default` собран против входа
`hyprland` этого флейка, так что он подходит, только если Hyprland запущен из того же входа
(добавьте `inputs.hyprland.follows = "hyprland"` и укажите его в `programs.hyprland.package`).
`packages.nixpkgs` собран против `hyprland` из nixpkgs, на котором залочен этот флейк. Есть и
`overlays.default` (`pkgs.hyprlandPlugins.noshare-cover`, собирается против `final.hyprland`).

Какой Hyprland запущен: `hyprctl version`.

## Проверка GPU

```sh
NOSHARE_COVER_DEBUG=/tmp/nsc.log Hyprland   # или экспортируйте в сессии
```

Поставьте `backend = "gpu"` и видео в `path_cover`, начните любой захват экрана и смотрите
`/tmp/nsc.log` и уведомление Hyprland: с `gpu` плагин на CPU не переходит, поэтому проблема
с видеокартой будет показана, а не спрятана. Полезно: `vainfo` (есть ли драйвер VA-API),
`nvidia-smi`.

## Разработка

```sh
cargo test                 # ядро: конфиг, часы, медиа, демукс, декодеры, реестр, API, раскладка ABI
NSC_TEST_MEDIA=dir cargo test   # + настоящие файлы: h264.mp4, av1.mp4, vp9.webm (декод и проверка)
tests/e2e/run.sh ./libnoshare-cover.so <каталог с роликами>        # живой Hyprland + grim, циклы unload/load
tests/e2e/with-gloview.sh ./libnoshare-cover.so ./gloview.so     # вместе с gloview
cargo clippy --all-targets -- -D warnings
make                       # libnoshare-cover.so (заголовки Hyprland через pkg-config; NSC_VAAPI=0 без VA-API)
nix build                  # герметичная сборка + тесты
```
