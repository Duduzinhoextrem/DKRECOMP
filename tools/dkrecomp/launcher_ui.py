"""Desktop launcher UI. All Tk calls and SDL diagnostics stay on the main thread."""
from __future__ import annotations

import ctypes
import os
from pathlib import Path
import queue
import shutil
import threading
import tkinter as tk
from tkinter import filedialog, messagebox, ttk

from . import launcher as app
from .controllers import BUTTONS, Gamepads

BG, SIDE, PANEL, BORDER = "#10191d", "#0b1215", "#19262c", "#2b3c42"
TEXT, MUTED, ACCENT, INK, WARN = "#f2f6f5", "#98adb3", "#ceef76", "#172018", "#f4c77b"


class Launcher:
    def __init__(self):
        if os.name == "nt":
            try:
                ctypes.windll.shcore.SetProcessDpiAwareness(1)
            except (OSError, AttributeError):
                pass
        self.root = tk.Tk()
        self.root.title("DKRecomp · Tropical Freeze")
        width, height = min(1180, self.root.winfo_screenwidth() - 70), min(820, self.root.winfo_screenheight() - 85)
        self.root.geometry(f"{width}x{height}")
        self.root.minsize(960, 650)
        self.root.configure(bg=BG)
        self.root.option_add("*Font", ("Segoe UI", 10))
        self.root.option_add("*TCombobox*Listbox.background", PANEL)
        self.root.option_add("*TCombobox*Listbox.foreground", TEXT)
        self.root.option_add("*TCombobox*Listbox.selectBackground", "#41563c")
        try:
            from PIL import Image, ImageDraw, ImageFont, ImageTk
            mark = Image.new("RGB", (64, 64), ACCENT)
            font_path = Path(os.environ.get("WINDIR", "C:/Windows")) / "Fonts/segoeuib.ttf"
            font = ImageFont.truetype(str(font_path), 29) if font_path.is_file() else ImageFont.load_default(size=25)
            ImageDraw.Draw(mark).text((32, 32), "DK", fill=INK, font=font, anchor="mm")
            self.icon = ImageTk.PhotoImage(mark)
            self.root.iconphoto(True, self.icon)
        except (ImportError, OSError):
            pass
        self.values = app.read_settings()
        self.saved_values = dict(self.values)
        self.messages = queue.Queue()
        self.busy = False
        self.game = None
        self.pads = None
        self.page_id = "home"
        self.pages, self.nav, self.refresh_widgets = {}, {}, []
        self.last_devices = []
        self.last_art_size = None
        self._style()
        self._layout()
        self._home()
        self._graphics()
        self._performance()
        self._audio()
        self._controls()
        self._installation()
        self.show("home")
        self.refresh()
        self.root.after(100, self.pump)
        self.root.after(100, self.poll_controllers)
        self.root.after(1000, self.monitor_game)
        self.root.protocol("WM_DELETE_WINDOW", self.close)
        self.root.report_callback_exception = self.callback_error

    def _style(self):
        style = ttk.Style(self.root)
        style.theme_use("clam")
        style.configure("TCombobox", fieldbackground=PANEL, background=PANEL, foreground=TEXT,
                        bordercolor=BORDER, lightcolor=BORDER, darkcolor=BORDER, arrowcolor=MUTED,
                        padding=(12, 9), selectbackground=PANEL, selectforeground=TEXT)
        style.map("TCombobox", fieldbackground=[("readonly", PANEL)], foreground=[("readonly", TEXT)],
                  selectbackground=[("readonly", PANEL)])
        style.configure("TCheckbutton", background=PANEL, foreground=TEXT, padding=5, indicatorcolor=BG)
        style.map("TCheckbutton", background=[("active", PANEL)], indicatorcolor=[("selected", ACCENT)])
        style.configure("Horizontal.TProgressbar", background=ACCENT, troughcolor=BG, bordercolor=BG,
                        lightcolor=ACCENT, darkcolor=ACCENT)
        style.configure("Vertical.TScrollbar", background=BORDER, troughcolor=BG, arrowcolor=MUTED,
                        bordercolor=BG, lightcolor=BG, darkcolor=BG)

    def label(self, parent, text, size=10, color=TEXT, bold=False, **kw):
        return tk.Label(parent, text=text, bg=parent.cget("bg"), fg=color,
                        font=("Segoe UI", size, "bold" if bold else "normal"), anchor="w", justify="left", **kw)

    def button(self, parent, text, command, primary=False, **kw):
        return tk.Button(parent, text=text, command=command, bg=ACCENT if primary else PANEL,
                         fg=INK if primary else TEXT, activebackground="#e2ffa0" if primary else "#2c4149",
                         activeforeground=INK if primary else TEXT, disabledforeground="#6d7d77", relief="flat",
                         bd=0, cursor="hand2", padx=18, pady=10, font=("Segoe UI", 10, "bold"), **kw)

    def _layout(self):
        sidebar = tk.Frame(self.root, bg=SIDE, width=204)
        sidebar.pack(side="left", fill="y")
        sidebar.pack_propagate(False)
        brand = tk.Frame(sidebar, bg=SIDE)
        brand.pack(fill="x", padx=24, pady=(30, 34))
        logo = tk.Canvas(brand, width=42, height=42, bg=SIDE, highlightthickness=0)
        logo.pack(side="left", padx=(0, 10))
        logo.create_rectangle(0, 0, 42, 42, fill=ACCENT, outline="")
        logo.create_text(21, 21, text="DK", fill=INK, font=("Segoe UI", 14, "bold"))
        self.label(brand, "DKRecomp", 13, bold=True).pack(anchor="w", pady=(0, 2))
        self.label(brand, "PC LAUNCHER", 8, MUTED).pack(anchor="w")
        self.label(sidebar, "BIBLIOTECA", 8, MUTED, True).pack(anchor="w", padx=25, pady=(0, 12))
        entries = [("home", "◈", "Visão geral"), ("graphics", "▣", "Gráficos"), ("performance", "↗", "Desempenho"),
                   ("audio", "♫", "Áudio"), ("controls", "⊕", "Controles"), ("install", "↓", "Instalação")]
        for key, icon, title in entries:
            button = tk.Button(sidebar, text=f"{icon}    {title}", anchor="w", command=lambda k=key: self.show(k),
                               relief="flat", bg=SIDE, fg=MUTED, activebackground=PANEL, activeforeground=TEXT,
                               cursor="hand2", bd=0, padx=17, pady=13, font=("Segoe UI", 10))
            button.pack(fill="x", padx=12, pady=2)
            self.nav[key] = button
        foot = tk.Frame(sidebar, bg=SIDE)
        foot.pack(side="bottom", fill="x", padx=25, pady=24)
        self.label(foot, "●  WINDOWS · VULKAN", 8, ACCENT, True).pack(anchor="w", pady=(0, 8))
        self.label(foot, "Recompilação nativa\nProjeto independente", 9, MUTED).pack(anchor="w")
        right = tk.Frame(self.root, bg=BG)
        right.pack(side="left", fill="both", expand=True)
        top = tk.Frame(right, bg=BG)
        top.pack(fill="x", padx=30, pady=(20, 16))
        self.breadcrumb = self.label(top, "BIBLIOTECA  /  TROPICAL FREEZE", 8, MUTED, True)
        self.breadcrumb.pack(side="left")
        self.top_status = self.label(top, "●  Verificando arquivos", 9, ACCENT)
        self.top_status.pack(side="right")
        footer = tk.Frame(right, bg=SIDE, height=66)
        footer.pack(side="bottom", fill="x")
        self.saved_label = self.label(footer, "Ajustes salvos", 9, MUTED)
        self.saved_label.pack(side="left", padx=26)
        self.save_button = self.button(footer, "Salvar ajustes", self.save, primary=True)
        self.save_button.pack(side="right", padx=(10, 26), pady=12)
        self.button(footer, "Restaurar padrão", self.restore).pack(side="right", pady=12)
        self.viewport = tk.Canvas(right, bg=BG, highlightthickness=0)
        scrollbar = ttk.Scrollbar(right, orient="vertical", command=self.viewport.yview)
        scrollbar.pack(side="right", fill="y", padx=(0, 6))
        self.viewport.pack(side="left", fill="both", expand=True)
        self.viewport.configure(yscrollcommand=scrollbar.set)
        self.page_host = tk.Frame(self.viewport, bg=BG)
        self.page_window = self.viewport.create_window(0, 0, window=self.page_host, anchor="nw")
        self.viewport.bind("<Configure>", self.viewport_resize)
        self.page_host.bind("<Configure>", lambda _e: self.viewport.configure(scrollregion=self.viewport.bbox("all")))
        self.root.bind("<MouseWheel>", self.mousewheel)

    def viewport_resize(self, event):
        self.viewport.itemconfigure(self.page_window, width=event.width)

    def mousewheel(self, event):
        if isinstance(event.widget, (ttk.Combobox, tk.Text)):
            return
        self.viewport.yview_scroll(-int(event.delta / 120), "units")

    def page(self, key, title, subtitle):
        frame = tk.Frame(self.page_host, bg=BG)
        self.pages[key] = frame
        self.label(frame, title, 25, bold=True).pack(fill="x", pady=(2, 5))
        self.label(frame, subtitle, 10, MUTED, wraplength=730).pack(fill="x", pady=(0, 24))
        return frame

    def card(self, parent, title=None, subtitle=None):
        outer = tk.Frame(parent, bg=PANEL, highlightbackground=BORDER, highlightthickness=1)
        outer.pack(fill="x", pady=(0, 16))
        inner = tk.Frame(outer, bg=PANEL)
        inner.pack(fill="both", expand=True, padx=22, pady=18)
        if title:
            self.label(inner, title, 12, bold=True).pack(fill="x", pady=(0, 4 if subtitle else 12))
        if subtitle:
            self.label(inner, subtitle, 9, MUTED, wraplength=700).pack(fill="x", pady=(0, 15))
        return inner

    def set_value(self, key, value):
        self.values[key] = value
        self.saved_label.configure(text="Alterações não salvas", fg=WARN)
        self.update_summary()

    def combo(self, parent, label, options, keys, hint=""):
        keys = (keys,) if isinstance(keys, str) else keys
        row = tk.Frame(parent, bg=PANEL)
        row.pack(fill="x", pady=8)
        left = tk.Frame(row, bg=PANEL)
        left.pack(side="left", fill="x", expand=True, padx=(0, 16))
        self.label(left, label, 10).pack(anchor="w")
        if hint:
            self.label(left, hint, 9, MUTED, wraplength=340).pack(anchor="w", pady=(4, 0))
        names = [o[0] for o in options]
        variable = tk.StringVar()
        box = ttk.Combobox(row, textvariable=variable, values=names, state="readonly", width=30)
        box.pack(side="right")

        def refresh():
            current = tuple(self.values[k] for k in keys)
            match = next((o[0] for o in options if tuple(o[1:]) == current), None)
            variable.set(match or "Personalizado: " + " × ".join(current))

        def select(_event):
            option = options[names.index(variable.get())]
            for key, value in zip(keys, option[1:]):
                self.set_value(key, value)

        box.bind("<<ComboboxSelected>>", select)
        self.refresh_widgets.append(refresh)
        refresh()
        return box

    def check(self, parent, label, key, hint=""):
        row = tk.Frame(parent, bg=PANEL)
        row.pack(fill="x", pady=8)
        variable = tk.BooleanVar(value=self.values[key] == "true")
        ttk.Checkbutton(row, text=label, variable=variable,
                        command=lambda: self.set_value(key, "true" if variable.get() else "false")).pack(anchor="w")
        if hint:
            self.label(row, hint, 9, MUTED, wraplength=690).pack(anchor="w", padx=5, pady=(3, 0))
        self.refresh_widgets.append(lambda: variable.set(self.values[key] == "true"))

    def _home(self):
        page = self.page("home", "Sua próxima aventura.", "Donkey Kong Country: Tropical Freeze  /  Nintendo Switch · versão 1.0.0")
        self.hero = tk.Canvas(page, height=295, bg=PANEL, highlightthickness=1, highlightbackground=BORDER)
        self.hero.pack(fill="x", pady=(0, 16))
        self.hero.bind("<Configure>", self.draw_hero)
        self.play = self.button(self.hero, "▶   Jogar", self.play_game, primary=True)
        self.hero.create_window(26, 226, window=self.play, width=215, height=44, anchor="nw", tags="play")
        line = tk.Frame(page, bg=BG)
        line.pack(fill="x", pady=(0, 20))
        self.home_status = self.label(line, "Verificando instalação…", 10, ACCENT)
        self.home_status.pack(side="left")
        self.button(line, "Gerenciar instalação  →", lambda: self.show("install")).pack(side="right")
        summary = tk.Frame(page, bg=BG)
        summary.pack(fill="x", pady=(0, 20))
        self.summary_labels = []
        for i, (title, key, note) in enumerate((("IMAGEM", "graphics", "Resolução interna"),
                                               ("DESEMPENHO", "performance", "Limite de quadros"),
                                               ("CONTROLES", "controls", "Xbox · PlayStation · Teclado"))):
            summary.columnconfigure(i, weight=1, uniform="summary")
            cell = tk.Frame(summary, bg=PANEL, highlightbackground=BORDER, highlightthickness=1, cursor="hand2")
            cell.grid(row=0, column=i, sticky="nsew", padx=(0, 12 if i < 2 else 0))
            self.label(cell, title, 8, MUTED, True).pack(anchor="w", padx=18, pady=(14, 7))
            value = self.label(cell, "", 16, bold=True)
            value.pack(anchor="w", padx=18)
            self.summary_labels.append(value)
            self.label(cell, note, 8, MUTED).pack(anchor="w", padx=18, pady=(4, 15))
            for widget in [cell] + cell.winfo_children():
                widget.bind("<Button-1>", lambda _e, target=key: self.show(target))
        quick = self.card(page, "Perfil rápido", "Escolha uma base e personalize os ajustes nas páginas ao lado.")
        profiles = tk.Frame(quick, bg=PANEL)
        profiles.pack(fill="x")
        for title, scale in (("720p · leve", "0.667"), ("1080p · original", "1.0"), ("4K · mais nitidez", "2.0")):
            self.button(profiles, title, lambda s=scale: self.profile(s)).pack(side="left", padx=(0, 10))
        links = tk.Frame(page, bg=BG)
        links.pack(fill="x", pady=(0, 14))
        self.button(links, "Pasta de saves", lambda: self.open_folder(Path(app.USER) / "saves")).pack(side="left", padx=(0, 10))
        self.button(links, "Abrir logs", self.open_logs).pack(side="left", padx=(0, 10))
        self.button(links, "Alterar imagem", self.choose_art).pack(side="right")

    def art_path(self):
        return next((p for p in (Path(app.USER) / "launcher_artwork", Path(app.ROOT) / "reports" / "launcher_cover.png",
                                 Path(app.ROOT) / "reports" / "launcher_cover.bmp",
                                 Path(app.ROOT) / "reports" / "frame_validated.bmp") if p.is_file()), None)

    def draw_hero(self, _event=None):
        width = max(500, self.hero.winfo_width())
        size = (width, 295)
        if self.last_art_size == size:
            return
        self.last_art_size = size
        self.hero.delete("art")
        path = self.art_path()
        if path:
            try:
                from PIL import Image, ImageDraw, ImageOps, ImageTk
                art_width = max(380, width - 280)
                with Image.open(path) as source:
                    picture = ImageOps.fit(source.convert("RGB"), (art_width, 295), method=Image.Resampling.LANCZOS)
                image = Image.new("RGB", size, PANEL)
                image.paste(picture, (width - art_width, 0))
                overlay = Image.new("RGBA", size)
                draw = ImageDraw.Draw(overlay)
                for x in range(width - art_width, min(width, width - art_width + 125)):
                    opacity = int(255 * (1 - (x - (width - art_width)) / 125))
                    draw.line((x, 0, x, 295), fill=(25, 38, 44, opacity))
                image = Image.alpha_composite(image.convert("RGBA"), overlay)
                self.art_photo = ImageTk.PhotoImage(image)
                self.hero.create_image(0, 0, image=self.art_photo, anchor="nw", tags="art")
            except (OSError, ImportError, ValueError):
                self.hero.create_text(width - 160, 145, text="Sua imagem do jogo", fill=MUTED, tags="art")
        else:
            self.hero.create_text(width - 180, 145, text="Escolha uma imagem do jogo\nem ‘Alterar imagem’", fill=MUTED, tags="art")
        for x, y, text, size, color, weight in (
            (27, 26, "SUA BIBLIOTECA · 01", 8, ACCENT, "bold"),
            (26, 67, "Donkey Kong\nCountry", 25, TEXT, "bold"),
            (28, 142, "Tropical Freeze", 15, ACCENT, "bold"),
            (28, 178, "Teclado, Xbox e PlayStation.\nAjustes para jogar do seu jeito.", 9, MUTED, "normal")):
            self.hero.create_text(x, y, text=text, fill=color, font=("Segoe UI", size, weight), anchor="nw", tags="art")
        self.hero.tag_lower("art", "play")

    def profile(self, scale):
        self.set_value("render_scale", scale)
        for key, value in (("fps_limit", "60"), ("vsync", "mailbox"), ("keep_aspect", "true"), ("present_filter", "linear")):
            self.set_value(key, value)
        for refresh in self.refresh_widgets:
            refresh()

    def _graphics(self):
        page = self.page("graphics", "Gráficos", "Escolha como o jogo aparece na sua tela.")
        card = self.card(page, "Tela")
        self.combo(card, "Modo de exibição", [("Janela", "windowed"), ("Tela cheia sem bordas", "borderless")],
                   "display_mode", "Alterne durante o jogo com Alt+Enter ou F11.")
        self.combo(card, "Tamanho da janela", app.WINDOW_PRESETS, ("window_width", "window_height"))
        self.check(card, "Manter proporção 16:9", "keep_aspect", "Preserva o formato da imagem em telas mais largas ou mais altas.")
        card = self.card(page, "Qualidade da imagem")
        self.combo(card, "Resolução interna", app.RENDER_PRESETS, "render_scale",
                   "1080p é o original. Resoluções maiores suavizam as bordas e exigem mais da GPU.")
        self.combo(card, "Filtro de escala", [("Suave · linear", "linear"), ("Nítido · nearest", "nearest")], "present_filter")
        self.label(page, "A qualidade 4K foi verificada nos menus. A fase ainda precisa de validação nessa resolução.", 9, MUTED,
                   wraplength=750).pack(fill="x", pady=(0, 16))

    def _performance(self):
        page = self.page("performance", "Desempenho", "Defina a fluidez e acompanhe a taxa de quadros.")
        card = self.card(page, "Taxa de quadros")
        self.combo(card, "Limite de FPS", app.FPS_PRESETS, "fps_limit", "60 FPS é o perfil original.")
        vsync = [("Mailbox · recomendado", "mailbox"), ("Ligado · FIFO", "on"), ("Desligado", "off")]
        self.combo(card, "Sincronização vertical", vsync, "vsync", "Mailbox evita cortes na imagem com baixa latência.")
        self.check(card, "Mostrar FPS na janela do jogo", "show_stats")
        card = self.card(page, "Sobre FPS acima de 60")
        self.label(card, "Opção experimental. As transições de menu foram medidas, mas a física e o ritmo da fase\n"
                         "ainda precisam de validação. Use 60 FPS para o perfil padrão.", 10, MUTED,
                   wraplength=700).pack(fill="x")

        card = self.card(page, "Cenas em vídeo", "Os vídeos originais são 720p a 29,97 fps.")
        self.check(card, "Suavizar vídeos para 59,94 fps (experimental)", "movie_smoothing",
                   "Mistura quadros vizinhos; pode criar rastros em movimentos. Não aumenta os detalhes do vídeo.")

    def _audio(self):
        page = self.page("audio", "Áudio", "Som do jogo e das cenas em vídeo.")
        card = self.card(page, "Saída de áudio")
        self.check(card, "Silenciar o jogo", "mute", "Silencia também os vídeos. O processamento de áudio continua ativo.")
        self.label(card, "Saída WASAPI com fila curta (30 ms), usando o dispositivo padrão do Windows.\n"
                         "O volume pode ser ajustado pelo mixer do Windows durante a partida.", 10, MUTED,
                   wraplength=700).pack(fill="x", pady=(12, 4))

    def _controls(self):
        page = self.page("controls", "Controles", "Conecte por USB ou Bluetooth e teste os comandos aqui.")
        card = self.card(page, "Teste do controle", "Os botões acendem ao pressionar. Mova os analógicos e aperte os gatilhos.")
        self.controller_name = self.label(card, "Procurando controles…", 11, ACCENT, True)
        self.controller_name.pack(fill="x")
        self.pad_canvas = tk.Canvas(card, width=720, height=210, bg=PANEL, highlightthickness=0)
        self.pad_canvas.pack(fill="x", pady=(10, 0))
        self.pad_canvas.bind("<Configure>", lambda _e: self.draw_pad(None))
        self.pad_note = self.label(card, "", 9, MUTED, wraplength=700)
        self.pad_note.pack(fill="x", pady=(0, 6))
        card = self.card(page, "Preferências de entrada")
        self.combo(card, "Sistema de controles", [("Automático · Xbox + PlayStation", "auto"),
                                                  ("XInput · Xbox / controle virtual", "xinput"),
                                                  ("Apenas teclado", "keyboard")], "input_backend")
        self.combo(card, "Controle do jogador 1 / teste", [("1 · primeiro controle", "0"), ("2 · segundo controle", "1"),
                                                    ("3 · terceiro controle", "2"), ("4 · quarto controle", "3")], "controller_index")
        self.combo(card, "Controle do jogador 2", [("Desativado", "4"), ("1 · primeiro controle", "0"),
                                                   ("2 · segundo controle", "1"), ("3 · terceiro controle", "2"),
                                                   ("4 · quarto controle", "3")], "second_controller_index",
                   "Escolha um controle diferente do jogador 1. O teclado controla apenas o jogador 1.")
        self.combo(card, "Zona morta dos analógicos", [(f"{n}%", str(n)) for n in (0, 5, 10, 15, 20, 25, 30, 40, 50)],
                   "stick_deadzone", "Aumente se o personagem se mover com o analógico parado.")
        self.check(card, "Trocar A/B e X/Y", "swap_abxy", "O padrão segue a posição física dos botões do Switch.")
        self.check(card, "Ler controle com o jogo em segundo plano", "input_background", "O teclado só funciona com a janela do jogo em foco.")
        card = self.card(page, "Guia de comandos", "Os nomes A, B, X e Y exibidos pelo jogo seguem o controle Nintendo.")
        rows = [("No jogo", "Xbox", "PlayStation", "Teclado"), ("Começar · L + R", "LB + RB", "L1 + R1", "U + O"),
                ("A · confirmar", "B", "○", "L / E"), ("B · pular / voltar", "A", "×", "Espaço / K"),
                ("Y · ataque / rolar", "X", "□", "J / Q"), ("X", "Y", "△", "I"),
                ("ZL / ZR", "LT / RT", "L2 / R2", "Shift / C"),
                ("+ · pausa", "Start / Menu", "Options", "Enter / Esc"),
                ("−", "Back / View", "Share / Create", "Tab / Backspace"),
                ("Mover", "Analógico / D-pad", "Analógico / D-pad", "WASD / setas")]
        table = tk.Frame(card, bg=PANEL)
        table.pack(fill="x")
        self.mapping_labels = []
        for r, row in enumerate(rows):
            for c, text in enumerate(row):
                label = self.label(table, text, 9, MUTED if r == 0 else TEXT, r == 0)
                label.grid(row=r, column=c, sticky="w", padx=(0, 22), pady=7)
                self.mapping_labels.append(label)
        self.label(page, "Suporte a um jogador. Vibração e sensores de movimento ainda não são enviados ao controle.",
                   9, MUTED, wraplength=740).pack(fill="x", pady=(0, 16))

    def draw_pad(self, device):
        c = self.pad_canvas
        c.delete("all")
        w = max(600, c.winfo_width())
        center = w / 2
        pressed = device.buttons if device else (False,) * 15
        axes = device.axes if device else (0,) * 6
        sony = bool(device and device.kind in (4, 5, 6))
        c.create_polygon(center-170, 40, center-105, 23, center+105, 23, center+170, 40,
                         center+205, 155, center+176, 184, center+110, 136, center-110, 136,
                         center-176, 184, center-205, 155, fill="#243740", outline="#425b63", width=2, smooth=True)
        for x, y, label, on in ((center+132, 100, "×" if sony else "A", pressed[0]),
                                (center+162, 70, "○" if sony else "B", pressed[1]),
                                (center+102, 70, "□" if sony else "X", pressed[2]),
                                (center+132, 40, "△" if sony else "Y", pressed[3]),
                                (center-20, 65, "−", pressed[4]), (center+20, 65, "+", pressed[6]),
                                (center-140, 15, "L1" if sony else "LB", pressed[9]),
                                (center+140, 15, "R1" if sony else "RB", pressed[10])):
            radius = 14 if len(label) == 1 else 19
            c.create_oval(x-radius, y-13, x+radius, y+13, fill=ACCENT if on else BG, outline=BORDER)
            c.create_text(x, y, text=label, fill=INK if on else TEXT, font=("Segoe UI", 9, "bold"))
        for x, index, stick_index in ((center-65, 0, 7), (center+65, 2, 8)):
            y = 124
            c.create_oval(x-28, y-28, x+28, y+28, fill=BG, outline=ACCENT if pressed[stick_index] else BORDER, width=2)
            px, py = x + axes[index] / 32767 * 15, y + axes[index+1] / 32767 * 15
            c.create_oval(px-12, py-12, px+12, py+12, fill=ACCENT if abs(axes[index]) + abs(axes[index+1]) > 5000 else "#536b72", outline="")
            c.create_text(x, 176, text=f"{axes[index]:+6d}  {axes[index+1]:+6d}", fill=MUTED, font=("Consolas", 8))
        for x, y, label, index in ((center-132, 52, "↑", 11), (center-132, 104, "↓", 12),
                                    (center-158, 78, "←", 13), (center-106, 78, "→", 14)):
            c.create_rectangle(x-12, y-12, x+12, y+12, fill=ACCENT if pressed[index] else BG, outline=BORDER)
            c.create_text(x, y, text=label, fill=INK if pressed[index] else TEXT)
        for x, axis, name in ((center-225, 4, "L2" if sony else "LT"), (center+215, 5, "R2" if sony else "RT")):
            c.create_rectangle(x, 38, x+10, 137, fill=BG, outline=BORDER)
            amount = max(0, axes[axis]) / 32767
            c.create_rectangle(x+1, 136-97*amount, x+9, 136, fill=ACCENT, outline="")
            c.create_text(x+5, 155, text=name, fill=MUTED, font=("Segoe UI", 8))
        if not device:
            c.create_text(center, 200, text="Aguardando conexão", fill=MUTED, font=("Segoe UI", 9))

    def _installation(self):
        page = self.page("install", "Instalação", "Prepare seus arquivos, atualize o jogo e acompanhe as operações.")
        card = self.card(page, "Arquivos do jogo")
        self.install_state = self.label(card, "", 10, TEXT)
        self.install_state.pack(fill="x", pady=(0, 14))
        row = tk.Frame(card, bg=PANEL)
        row.pack(fill="x")
        self.setup_button = self.button(row, "Preparar a partir do NSP…", self.setup, primary=True)
        self.setup_button.pack(side="left", padx=(0, 10))
        self.build_button = self.button(row, "Atualizar / compilar", self.build)
        self.build_button.pack(side="left", padx=(0, 10))
        self.input_button = self.button(row, "Suporte a controles", self.install_input)
        self.input_button.pack(side="left")
        self.job_label = self.label(page, "Nenhuma operação em andamento", 10, MUTED, wraplength=740)
        self.job_label.pack(fill="x", pady=(2, 10))
        self.progress = ttk.Progressbar(page, mode="indeterminate")
        self.progress.pack(fill="x", pady=(0, 14))
        self.log_box = tk.Text(page, height=14, bg=SIDE, fg="#b2c7ca", insertbackground=TEXT,
                               relief="flat", bd=0, padx=15, pady=14, font=("Consolas", 9), wrap="word", state="disabled")
        self.log_box.pack(fill="both", expand=True, pady=(0, 16))
        self.button(page, "Abrir pasta de logs", self.open_logs).pack(anchor="w", pady=(0, 20))

    def show(self, key):
        for page in self.pages.values():
            page.pack_forget()
        self.pages[key].pack(fill="both", expand=True, padx=30, pady=(0, 14))
        self.page_id = key
        self.viewport.yview_moveto(0)
        for k, button in self.nav.items():
            button.configure(bg=PANEL if k == key else SIDE, fg=ACCENT if k == key else MUTED)
        if key != "controls" and self.pads:
            self.pads.close()
            self.pads = None

    def update_summary(self):
        if not hasattr(self, "summary_labels"):
            return
        scale = float(self.values["render_scale"])
        resolution = round(1080 * scale)
        self.summary_labels[0].configure(text="4K" if abs(scale-2) < 0.001 else f"{resolution}p")
        fps = self.values["fps_limit"]
        self.summary_labels[1].configure(text="Sem limite" if fps == "0" else f"{fps} FPS")
        self.summary_labels[2].configure(text="Teclado" if self.values["input_backend"] == "keyboard" else "Pronto para conectar")
        if hasattr(self, "mapping_labels"):
            swapped = self.values["swap_abxy"] == "true"
            labels = [("A", "×"), ("B", "○"), ("Y", "△"), ("X", "□")] if swapped else [("B", "○"), ("A", "×"), ("X", "□"), ("Y", "△")]
            for row, (xbox, sony) in zip((2, 3, 4, 5), labels):
                self.mapping_labels[row*4+1].configure(text=xbox)
                self.mapping_labels[row*4+2].configure(text=sony)

    def refresh(self):
        st = app.status()
        running = self.game is not None and self.game.poll() is None
        text = "Jogo em execução" if running else "Atualizando jogo…" if self.busy else "Pronto para jogar" if st.ready and not st.stale else "Atualização disponível" if st.ready else "Preparação necessária"
        color = ACCENT if st.ready and not st.stale else WARN
        self.top_status.configure(text="●  " + text, fg=color)
        self.home_status.configure(text="●  " + text, fg=color)
        self.play.configure(text="Em execução" if running else "Aguarde…" if self.busy else "▶   Atualizar e jogar" if st.stale else "▶   Jogar",
                            state="disabled" if self.busy or running or not (st.exefs and st.romfs) else "normal")
        items = (("Código do jogo", st.exefs), ("Dados do jogo", st.romfs), ("Executável nativo", st.built and st.images),
                 ("Tradutor de shaders", st.translator), ("Xbox e PlayStation", (Path(app.ROOT) / "toolchain/sdl3/SDL3.dll").is_file()))
        self.install_state.configure(text="\n".join(f"{'✓' if ok else '○'}  {title}   ·   {'pronto' if ok else 'pendente'}" for title, ok in items))
        for button in (self.setup_button, self.build_button, self.input_button):
            button.configure(state="disabled" if self.busy or running else "normal")
        self.build_button.configure(state="normal" if st.exefs and not self.busy and not running else "disabled")
        self.update_summary()

    def log(self, text):
        self.messages.put(("log", text))

    def pump(self):
        for _ in range(200):
            try:
                kind, payload = self.messages.get_nowait()
            except queue.Empty:
                break
            if kind == "log":
                self.log_box.configure(state="normal")
                self.log_box.insert("end", payload)
                if int(self.log_box.index("end-1c").split(".")[0]) > 1600:
                    self.log_box.delete("1.0", "400.0")
                self.log_box.see("end")
                self.log_box.configure(state="disabled")
                if payload.startswith("\n== "):
                    self.job_label.configure(text=payload.strip()[3:], fg=TEXT)
            elif kind == "done":
                success, play_after = payload
                self.busy = False
                self.progress.stop()
                self.job_label.configure(text="Pronto. Operação concluída." if success else "A operação falhou. Confira os detalhes abaixo.", fg=ACCENT if success else WARN)
                self.refresh()
                if success and play_after:
                    self.start_game()
        self.root.after(100, self.pump)

    def background(self, steps, play_after=False):
        if self.busy or (self.game and self.game.poll() is None):
            return
        self.busy = True
        self.show("install")
        self.progress.start(15)
        self.refresh()

        def work():
            try:
                ok = app.run_steps(steps, self.log)
            except Exception as error:
                self.log(f"\nErro: {error}\n")
                ok = False
            self.messages.put(("done", (ok, play_after)))
        threading.Thread(target=work, daemon=True).start()

    def save(self):
        try:
            app.write_settings(self.values)
        except (OSError, ValueError) as error:
            messagebox.showerror("Não foi possível salvar", str(error), parent=self.root)
            return False
        self.saved_values = dict(self.values)
        self.saved_label.configure(text="Ajustes salvos", fg=ACCENT)
        return True

    def restore(self):
        self.values = dict(app.DEFAULTS)
        for refresh in self.refresh_widgets:
            refresh()
        self.saved_label.configure(text="Padrão restaurado · clique em Salvar ajustes", fg=WARN)
        self.update_summary()

    def play_game(self):
        if self.busy or (self.game and self.game.poll() is None) or not self.save():
            return
        st = app.status()
        if not st.exefs or not st.romfs:
            self.show("install")
            return
        if st.stale or not st.ready:
            self.background(app.build_steps(), play_after=True)
        else:
            self.start_game()

    def start_game(self):
        if self.pads:
            self.pads.close()
            self.pads = None
        try:
            self.game = app.launch()
        except (OSError, RuntimeError) as error:
            messagebox.showerror("Não foi possível iniciar", str(error), parent=self.root)
            return
        self.log("\nJogo iniciado. Logs em generated/dkctf_v0.\n")
        self.refresh()
        self.root.iconify()

    def monitor_game(self):
        if self.game and self.game.poll() is not None:
            code = self.game.returncode
            self.game = None
            self.root.deiconify()
            self.refresh()
            self.log(f"\nJogo encerrado (código {code}).\n")
            if code not in (0,):
                self.show("install")
                reason = app.last_failure()
                self.job_label.configure(text="O jogo encerrou com um erro: " + reason, fg=WARN)
                self.log("Motivo informado pelo runtime: " + reason + "\n")
        self.root.after(1000, self.monitor_game)

    def build(self):
        self.background(app.build_steps())

    def install_input(self):
        if self.pads:
            self.pads.close()
            self.pads = None
        self.background([("Preparando suporte a Xbox e PlayStation", [app.PYTHON, str(Path(app.ROOT) / "tools/fetch_input_deps.py")],
                          str(Path(app.ROOT) / "toolchain/sdl3/SDL3.dll"))])

    def setup(self):
        nsp = filedialog.askopenfilename(parent=self.root, title="Escolha o NSP do jogo base (v0)", filetypes=[("Jogo NSP", "*.nsp")],
                                         initialdir=Path(app.ROOT) / "game original meu")
        if not nsp:
            return
        try:
            nca, ticket, release, title = app.nsp_plan(nsp)
            if title.lower() != "0100c1f0051b6000":
                raise ValueError("Escolha o jogo base 0100C1F0051B6000, versão 1.0.0 (v0).")
        except Exception as error:
            messagebox.showerror("NSP não utilizável", str(error), parent=self.root)
            return
        st = app.status()
        nstool = keys = ""
        if not st.exefs or not st.romfs:
            nstool = app.default_tool("nstool.exe") or filedialog.askopenfilename(parent=self.root, title="Selecione seu nstool.exe", filetypes=[("nstool", "nstool.exe")])
            if not nstool:
                return
            keys = app.default_tool("prod.keys") or filedialog.askopenfilename(parent=self.root, title="Selecione seu prod.keys", filetypes=[("Chaves", "*.keys")])
            if not keys:
                return
        need = (21 << 30) if not st.romfs else 1 << 30
        if shutil.disk_usage(app.ROOT).free < need:
            messagebox.showerror("Espaço insuficiente", f"Libere pelo menos {need >> 30} GB para preparar o jogo.", parent=self.root)
            return
        self.log(f"\nPreparando {Path(nsp).name} · {release}\n")
        self.background(app.setup_steps(nsp, nstool, keys, nca, ticket, redo=False))

    def poll_controllers(self):
        if self.page_id == "controls" and not self.busy and not (self.game and self.game.poll() is None):
            try:
                if not self.pads:
                    self.pads = Gamepads()
                devices = self.pads.poll()
                self.last_devices = devices
                index = int(self.values["controller_index"])
                device = devices[index] if index < len(devices) else None
                self.controller_name.configure(text=f"●  {device.name}" if device else "○  Nenhum controle neste slot", fg=ACCENT if device else MUTED)
                self.draw_pad(device)
                if device:
                    active = [name for name, down in zip(BUTTONS, device.buttons) if down]
                    mode = " · Jogo configurado apenas para teclado" if self.values["input_backend"] == "keyboard" else " · Jogo configurado para XInput" if self.values["input_backend"] == "xinput" else ""
                    self.pad_note.configure(text="Teste SDL3 · Pressionados: " + (" · ".join(active) or "nenhum") + f"   |   {len(devices)} controle(s)" + mode)
                else:
                    self.pad_note.configure(text="Conecte o controle por USB ou faça o pareamento Bluetooth no Windows.")
            except (OSError, RuntimeError, AttributeError) as error:
                self.controller_name.configure(text="Suporte a controles indisponível", fg=WARN)
                self.pad_note.configure(text="Use ‘Suporte a controles’ na página Instalação. " + str(error)[:130])
        self.root.after(50 if self.page_id == "controls" else 400, self.poll_controllers)

    def open_folder(self, path):
        try:
            Path(path).mkdir(parents=True, exist_ok=True)
            os.startfile(path)
        except OSError as error:
            messagebox.showerror("Não foi possível abrir a pasta", str(error), parent=self.root)

    def open_logs(self):
        self.open_folder(Path(app.GENERATED))

    def choose_art(self):
        path = filedialog.askopenfilename(parent=self.root, title="Escolha uma imagem do jogo", filetypes=[("Imagens", "*.png *.jpg *.jpeg *.webp *.bmp")])
        if not path:
            return
        try:
            from PIL import Image
            with Image.open(path) as image:
                image.verify()
            destination = Path(app.USER) / "launcher_artwork"
            destination.parent.mkdir(parents=True, exist_ok=True)
            if Path(path).resolve() != destination.resolve():
                shutil.copyfile(path, destination)
            self.last_art_size = None
            self.draw_hero()
        except (OSError, ValueError, ImportError) as error:
            messagebox.showerror("Imagem não utilizável", str(error), parent=self.root)

    def callback_error(self, kind, value, trace):
        import traceback
        folder = Path(app.ROOT) / "reports"
        folder.mkdir(exist_ok=True)
        with (folder / "launcher_error.log").open("a", encoding="utf-8") as output:
            traceback.print_exception(kind, value, trace, file=output)
        messagebox.showerror("Erro no launcher", f"{value}\n\nDetalhes em reports/launcher_error.log", parent=self.root)

    def close(self):
        if self.busy:
            self.root.iconify()
            return
        if self.values != self.saved_values and not self.save():
            return
        if self.pads:
            self.pads.close()
        self.root.destroy()

    def run(self):
        if os.environ.get("DKR_LAUNCHER_SMOKE"):
            def smoke():
                for key in self.pages:
                    self.show(key)
                    self.root.update_idletasks()
                self.close()
            self.root.after(500, smoke)
        self.root.mainloop()
        return 0
