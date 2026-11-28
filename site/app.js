(() => {
  const REPO = "77450danik/project-arctic";

  const I18N = {
    uk: {
      pageTitle: "Project Arctic — Next-Gen Universal Hybrid Operating System for PC",
      "nav.features": "Особливості",
      "nav.screens": "Знімки",
      "nav.download": "Завантажити",
      "hero.title": "Стабільність, комфорт і&nbsp;універсальність в&nbsp;одному — це&nbsp;Arctic.",
      "hero.tagline": "Універсальна гібридна операційна система нового покоління для ПК",
      "hero.lead": "Готова до роботи одразу: не треба встановлювати драйвери й програми.",
      "hero.download": "Завантажити ISO",
      "hero.dark": "Темна",
      "hero.light": "Світла",
      "hero.desktop": "Робочий стіл",
      "features.title": "Особливості",
      "f.chrome": "Chrome замість Edge",
      "f.chromeDesc": "Замість непотрібного Edge — Google Chrome, оптимізований для Arctic з коробки. Браузер, яким користується більшість і який зручний для всіх.",
      "f.fast": "Швидка",
      "f.fastDesc": "Виклики NT і вікна обробляє ядро, композитор працює на Vulkan.",
      "f.ramNum": "0,8 ГБ",
      "f.ramDesc": "ОЗП зайнято після старту (ВМ, 6 ГБ).",
      "f.work": "Для роботи",
      "f.workDesc": "Провідник із вкладками, Chrome, PowerShell 7, Git, WSL, KVM.",
      "f.games": "Для ігор",
      "f.gamesDesc": "DXVK і vkd3d-proton, оверлей FPS, андервольт ЦП і розгін ГП.",
      "f.drivers": "Драйвери",
      "f.driversDesc": "Intel, AMD і NVIDIA, Wi-Fi, звук і мережа працюють одразу з Live USB.",
      "f.core": "Ядро",
      "f.coreDesc": "NT-модулі в ядрі Linux, WinHvPlatform на KVM, Secure Boot, диск C: на NTFS.",
      "apps.label": "Перевірено:",
      "screens.title": "Знімки",
      "screens.note": "Знімки з QEMU, 1920×1080.",
      "s.explorer": "Провідник",
      "s.snap": "Snap",
      "s.themes": "Персоналізація",
      "s.taskmgr": "Диспетчер задач",
      "s.explorerCap": "Вкладки, швидкий доступ, диск C: на NTFS.",
      "s.snapCap": "Вікна поруч, композитор dwm на Vulkan.",
      "s.chromeCap": "Chrome — браузер за замовчуванням, Edge немає.",
      "s.wordpadCap": "Власний редактор зі стрічкою: DOCX, RTF, ODT, TXT.",
      "s.themesCap": "Світла, темна й змішана теми, значки, стилі «Пуску» і панелі задач.",
      "s.pwshCap": "PowerShell 7 і вікно «Про Arctic».",
      "s.taskmgrCap": "Диспетчер задач: 0,8 ГБ ОЗП після старту.",
      "dl.caption": "Завантаження",
      "dl.title": "Завантажити Arctic",
      "dl.all": "Усі релізи",
      "dl.usbKey": "Флешка",
      "dl.usbVal": "запис ISO через Rufus",
      "dl.status": "Стан",
      "dl.boot": "Завантаження",
      "dl.bootVal": "UEFI (Secure Boot з ARCTIC.cer), BIOS",
      "dl.ram": "ОЗП",
      "dl.ramVal": "від 4 ГБ",
      "footer.docs": "Документація",
      "footer.releases": "Релізи",
      "footer.about": "Гібридна операційна система для ПК: Windows-програми й ігри на ядрі NT усередині Linux.",
      "footer.system": "Система",
      "footer.dev": "Розробка",
      "footer.help": "Допомога",
      "footer.images": "Образи",
      "footer.install": "Встановлення",
      "footer.build": "Збірка з джерел",
      "footer.bug": "Повідомити про помилку",
      "footer.copy": "© 2026 Project Arctic. Не пов'язано з Microsoft.",
      "footer.credit": "Фото гір: Willi Winzig, CC BY 2.0",
      theme: "Тема",
      gb: "ГБ"
    },
    en: {
      pageTitle: "Project Arctic — Next-Gen Universal Hybrid Operating System for PC",
      "nav.features": "Features",
      "nav.screens": "Screenshots",
      "nav.download": "Download",
      "hero.title": "Stability, comfort and versatility in&nbsp;one — this is&nbsp;Arctic.",
      "hero.tagline": "Next-Gen Universal Hybrid Operating System for PC",
      "hero.lead": "Ready to work right away: no drivers or apps to install.",
      "hero.download": "Download ISO",
      "hero.dark": "Dark",
      "hero.light": "Light",
      "hero.desktop": "Desktop",
      "features.title": "Features",
      "f.chrome": "Chrome instead of Edge",
      "f.chromeDesc": "Instead of the unneeded Edge — Google Chrome, optimized for Arctic out of the box. The browser most people use, comfortable for everyone.",
      "f.fast": "Fast",
      "f.fastDesc": "NT calls and windowing run in the kernel; the compositor runs on Vulkan.",
      "f.ramNum": "0.8 GB",
      "f.ramDesc": "RAM in use after boot (VM, 6 GB).",
      "f.work": "For work",
      "f.workDesc": "Tabbed File Explorer, Chrome, PowerShell 7, Git, WSL, KVM.",
      "f.games": "For games",
      "f.gamesDesc": "DXVK and vkd3d-proton, FPS overlay, CPU undervolt and GPU overclock.",
      "f.drivers": "Drivers",
      "f.driversDesc": "Intel, AMD and NVIDIA, Wi-Fi, audio and network work straight from Live USB.",
      "f.core": "Kernel",
      "f.coreDesc": "NT modules in the Linux kernel, WinHvPlatform on KVM, Secure Boot, NTFS C: drive.",
      "apps.label": "Tested:",
      "screens.title": "Screenshots",
      "screens.note": "Captured in QEMU, 1920×1080.",
      "s.explorer": "File Explorer",
      "s.snap": "Snap",
      "s.themes": "Personalization",
      "s.taskmgr": "Task Manager",
      "s.explorerCap": "Tabs, Quick access, an NTFS C: drive.",
      "s.snapCap": "Windows side by side, dwm compositing on Vulkan.",
      "s.chromeCap": "Chrome is the default browser; there is no Edge.",
      "s.wordpadCap": "Arctic's own ribbon editor: DOCX, RTF, ODT, TXT.",
      "s.themesCap": "Light, dark and mixed modes, icon sets, Start and taskbar styles.",
      "s.pwshCap": "PowerShell 7 and the About Arctic dialog.",
      "s.taskmgrCap": "Task Manager: 0.8 GB of RAM after boot.",
      "dl.caption": "Download",
      "dl.title": "Download Arctic",
      "dl.all": "All releases",
      "dl.usbKey": "USB stick",
      "dl.usbVal": "write the ISO with Rufus",
      "dl.status": "Status",
      "dl.boot": "Boot",
      "dl.bootVal": "UEFI (Secure Boot with ARCTIC.cer), BIOS",
      "dl.ram": "RAM",
      "dl.ramVal": "4 GB or more",
      "footer.docs": "Docs",
      "footer.releases": "Releases",
      "footer.about": "A hybrid operating system for PC: Windows apps and games on an NT core inside Linux.",
      "footer.system": "System",
      "footer.dev": "Development",
      "footer.help": "Help",
      "footer.images": "Images",
      "footer.install": "Installation",
      "footer.build": "Build from source",
      "footer.bug": "Report a bug",
      "footer.copy": "© 2026 Project Arctic. Not affiliated with Microsoft.",
      "footer.credit": "Mountain photo: Willi Winzig, CC BY 2.0",
      theme: "Theme",
      gb: "GB"
    }
  };

  let lang = "uk";
  let release = null;
  let shotCapKey = "s.explorerCap";

  /* ---------- language ---------- */
  function pickLang() {
    const q = new URLSearchParams(location.search).get("lang");
    if (q === "en" || q === "uk") return q;
    try {
      const saved = localStorage.getItem("arctic_lang");
      if (saved === "en" || saved === "uk") return saved;
    } catch (_) {}
    // crawlers index the page as written (Ukrainian)
    if (/bot|crawl|spider|slurp/i.test(navigator.userAgent)) return "uk";
    return /^(uk|ru|be)\b/i.test(navigator.language || "") ? "uk" : "en";
  }

  function setLang(next) {
    lang = next === "en" ? "en" : "uk";
    const t = I18N[lang];
    document.documentElement.lang = lang;
    document.title = t.pageTitle;
    document.querySelectorAll("[data-i18n]").forEach((el) => {
      const v = t[el.dataset.i18n];
      if (v) el.textContent = v;
    });
    document.querySelectorAll("[data-i18n-html]").forEach((el) => {
      const v = t[el.dataset.i18nHtml];
      if (v) el.innerHTML = v;
    });
    const cap = document.getElementById("shot-cap");
    if (cap && t[shotCapKey]) cap.textContent = t[shotCapKey];
    document.querySelectorAll("[data-lang]").forEach((b) => b.classList.toggle("is-active", b.dataset.lang === lang));
    const tb = document.getElementById("theme-toggle");
    if (tb) { tb.title = t.theme; tb.setAttribute("aria-label", t.theme); }
    renderRelease();
    try { localStorage.setItem("arctic_lang", lang); } catch (_) {}
  }

  /* ---------- latest image ----------
     latest.json (next to this page) names the newest image wherever it is
     hosted; without it, the newest GitHub release that has arctic.iso. */
  function formatSize(bytes) {
    const gb = bytes / 1024 ** 3;
    return gb.toLocaleString(lang === "uk" ? "uk-UA" : "en-US", { maximumFractionDigits: 2, minimumFractionDigits: 2 }) + " " + I18N[lang].gb;
  }

  function formatDate(iso) {
    const d = new Date(iso);
    if (isNaN(d)) return "";
    return d.toLocaleDateString(lang === "uk" ? "uk-UA" : "en-US", { day: "numeric", month: "long", year: "numeric" });
  }

  function renderRelease() {
    if (!release) return;
    const parts = [release.name];
    if (release.date) parts.push(formatDate(release.date));
    if (release.size) parts.push(formatSize(release.size));
    document.querySelectorAll(".js-release-line").forEach((el) => {
      el.textContent = parts.filter(Boolean).join(" · ");
    });
  }

  function applyRelease(r) {
    release = r;
    document.querySelectorAll(".js-iso-link").forEach((a) => { a.href = r.iso; });
    if (r.sha256) {
      document.querySelectorAll(".js-sha").forEach((el) => { el.textContent = r.sha256; });
      document.querySelectorAll(".js-sha-row").forEach((el) => { el.hidden = false; });
    }
    renderRelease();
  }

  async function fromManifest() {
    const res = await fetch("latest.json", { cache: "no-cache" });
    if (!res.ok) return null;
    const m = await res.json();
    if (!m || !m.iso || !m.iso.url) return null;
    return {
      name: m.name || "Arctic",
      date: m.date,
      size: m.iso.size,
      sha256: m.iso.sha256,
      iso: m.iso.url
    };
  }

  async function fromGitHub() {
    const res = await fetch(`https://api.github.com/repos/${REPO}/releases?per_page=10`, {
      headers: { Accept: "application/vnd.github+json" }
    });
    if (!res.ok) return null;
    const list = await res.json();
    if (!Array.isArray(list)) return null;
    const rel = list.find((r) => !r.draft && (r.assets || []).some((a) => a.name === "arctic.iso"));
    if (!rel) return null;
    const iso = rel.assets.find((a) => a.name === "arctic.iso");
    return {
      name: rel.name || rel.tag_name,
      date: iso.updated_at || rel.published_at,
      size: iso.size,
      iso: iso.browser_download_url
    };
  }

  async function loadRelease() {
    for (const source of [fromManifest, fromGitHub]) {
      try {
        const r = await source();
        if (r) { applyRelease(r); return; }
      } catch (_) {}
    }
  }

  /* ---------- theme ---------- */
  const heroImg = document.getElementById("hero-img");
  const media = window.matchMedia ? window.matchMedia("(prefers-color-scheme: light)") : null;
  let heroPicked = false;

  function currentTheme() {
    return document.documentElement.dataset.theme === "light" ? "light" : "dark";
  }

  function setHero(src) {
    if (heroImg) heroImg.src = src;
    document.querySelectorAll("[data-hero-src]").forEach((b) => b.classList.toggle("is-active", b.dataset.heroSrc === src));
  }

  function setTheme(theme, persist) {
    if (theme === "light") document.documentElement.dataset.theme = "light";
    else delete document.documentElement.dataset.theme;
    const meta = document.querySelector('meta[name="theme-color"]');
    if (meta) meta.content = theme === "light" ? "#f3f3f3" : "#0b0f17";
    if (!heroPicked) setHero(theme === "light" ? "assets/start-light.webp" : "assets/hero-start-dark.webp");
    if (persist) { try { localStorage.setItem("arctic_theme", theme); } catch (_) {} }
  }

  let savedTheme = null;
  try { savedTheme = localStorage.getItem("arctic_theme"); } catch (_) {}
  setTheme(savedTheme || (media && media.matches ? "light" : "dark"), false);

  if (media) {
    media.addEventListener("change", () => {
      let saved = null;
      try { saved = localStorage.getItem("arctic_theme"); } catch (_) {}
      if (!saved) setTheme(media.matches ? "light" : "dark", false);
    });
  }

  const themeBtn = document.getElementById("theme-toggle");
  if (themeBtn) themeBtn.addEventListener("click", () => setTheme(currentTheme() === "light" ? "dark" : "light", true));

  /* ---------- controls ---------- */
  document.querySelectorAll("[data-lang]").forEach((b) => b.addEventListener("click", () => setLang(b.dataset.lang)));

  document.querySelectorAll("[data-hero-src]").forEach((b) => {
    b.addEventListener("click", () => { heroPicked = true; setHero(b.dataset.heroSrc); });
  });

  const shotImg = document.getElementById("shot-img");
  document.querySelectorAll(".tab").forEach((tab) => {
    tab.addEventListener("click", () => {
      document.querySelectorAll(".tab").forEach((t) => {
        t.classList.toggle("is-active", t === tab);
        t.setAttribute("aria-selected", t === tab ? "true" : "false");
      });
      if (shotImg) shotImg.src = tab.dataset.img;
      shotCapKey = tab.dataset.cap;
      const cap = document.getElementById("shot-cap");
      if (cap) cap.textContent = I18N[lang][shotCapKey] || "";
    });
  });

  /* ---------- lightbox ---------- */
  const box = document.getElementById("lightbox");
  const boxImg = document.getElementById("lightbox-img");
  function closeBox() { if (box) { box.hidden = true; boxImg.src = ""; } }
  document.querySelectorAll(".zoomable").forEach((img) => {
    img.addEventListener("click", () => {
      boxImg.src = img.currentSrc || img.src;
      boxImg.alt = img.alt;
      box.hidden = false;
    });
  });
  document.getElementById("lightbox-close").addEventListener("click", closeBox);
  box.addEventListener("click", (e) => { if (e.target === box) closeBox(); });
  document.addEventListener("keydown", (e) => { if (e.key === "Escape") closeBox(); });

  setLang(pickLang());
  loadRelease();
})();
