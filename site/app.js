(() => {
  const I18N = {
    uk: {
      pageTitle: "Project Arctic — Гібридна ОС нового покоління для x86",
      "nav.experience": "Можливості",
      "nav.performance": "Продуктивність",
      "nav.architecture": "Архітектура",
      "nav.download": "Завантажити",
      "nav.cta": "Завантажити ISO",
      "hero.eyebrow": "Операційна система нового покоління для x86",
      "hero.title": "Знайомий комфорт.<br/>Нова швидкість.",
      "hero.subtitle": "Унікальна гібридна операційна система для x86, що поєднує найвищі результати продуктивності, нативну підтримку Windows-програм і бездоганну зручність щоденного користування.",
      "hero.download": "Завантажити Live CD (ISO)",
      "hero.github": "Відкрити на GitHub",
      "hero.meta": "Останній реліз Live CD · x86-64 · UEFI Secure Boot та BIOS",
      "hero.modeDark": "Темний режим",
      "hero.modeLight": "Світлий режим",
      "hero.modeDesktop": "Робочий стіл",
      "common.vmNote": "Реальний знімок екрана з віртуальної машини QEMU (1920×1080)",
      "metrics.title": "Створена для максимальної швидкодії",
      "metrics.subtitle": "Підсистема Windows NT працює безпосередньо на рівні ядра — без зайвих прошарків та втрат.",
      "metrics.gb": "ГБ",
      "metrics.us": "мкс",
      "metrics.ramLabel": "Споживання ОЗП у спокої",
      "metrics.ramDesc": "Швидкий холодний старт та легка система, готова до ігор і роботи.",
      "metrics.win32kLabel": "win32k у ядрі (4.6× швидше)",
      "metrics.win32kDesc": "Створення та керування вікнами напряму в потоці програми через ядро.",
      "metrics.fastpathLabel": "Швидкі шляхи NT-реєстру",
      "metrics.fastpathDesc": "Спільна пам’ять і кешування ключів без очікування міжпроцесного обміну.",
      "metrics.gfxLabel": "Нативний композитор DWM",
      "metrics.gfxDesc": "Прямий вивід на atomic KMS, апаратний курсор, VRR і розмиття Acrylic на GPU.",
      "showcase.title": "Зручність, яку відчуваєш одразу",
      "showcase.subtitle": "Знайомий дизайн у стандартах Windows 11: закруглені вікна, вкладки, ефект Acrylic і миттєвий відгук.",
      "showcase.tab1": "Провідник із вкладками",
      "showcase.tab2": "Багатовіконність і Snap",
      "showcase.tab3": "Диспетчер завдань",
      "showcase.tab4": "Персоналізація та теми",
      "showcase.tab1Desc": "Провідник у стилі Windows 11 із вкладками, швидким доступом і нативним диском C: на NTFS.",
      "showcase.tab2Desc": "Зручне прикріплення вікон поруч у темному режимі з апаратним композитингом dwm.exe.",
      "showcase.tab3Desc": "Реальний моніторинг ЦП, пам’яті, фізичних накопичувачів SSD/HDD, мережі та відеокарти.",
      "showcase.tab4Desc": "Вибір світлого, темного чи комбінованого оформлення, прозорості заголовків та стилю елементів.",
      "gallery.title": "Світла й темна гармонія",
      "gallery.subtitle": "Реальні кадри роботи системи у віртуальній машині — від системних діалогів до PowerShell 7 і календаря.",
      "gallery.card1Title": "PowerShell 7 і «Про Arctic»",
      "gallery.card1Desc": "Нативні інструменти розробника та системний діалог версії в темному оформленні.",
      "gallery.card2Title": "Центр сповіщень і календар",
      "gallery.card2Desc": "Плавні заокруглені панелі сповіщень, календаря, звуку та мережі з розмиттям Acrylic.",
      "gallery.card3Title": "Гнучка персоналізація",
      "gallery.card3Desc": "Миттєве перемикання світлого, темного й комбінованого режимів, ефектів заголовків і значків.",
      "bento.title": "Справжній гібрид без компромісів",
      "bento.subtitle": "Єдиний світ Windows NT поверх сучасного ядра Linux — без віртуальних прошарків поверх робочого столу.",
      "bento.c1Title": "Гібридне ядро NT + Linux",
      "bento.c1Desc": "Модулі ядра win32k, ntos, ntimage та ntsync виконують системні виклики Windows напряму. Ніякого X11 чи стороннього Wayland-композитора — екран належить dwm.exe, а ввід — csrss.exe.",
      "bento.c2Title": "Ігри, GPU та KVM",
      "bento.c2Desc": "Direct3D 8–12 через DXVK і vkd3d-proton, Vulkan, CUDA, нативна підтримка WinHvPlatform на KVM та контейнери WSL.",
      "bento.c3Title": "Live CD, Live USB та Secure Boot",
      "bento.c3Desc": "Запуск безпосередньо з Live CD ISO або з флешки зі збереженням на реальний диск C: (NTFS), підписом Secure Boot та відкатом оновлень.",
      "download.title": "Спробуйте Project Arctic вже зараз",
      "download.subtitle": "Завантажте актуальний Live CD образ для віртуальної машини або сучасного ПК x86-64.",
      "download.isoBtn": "Завантажити arctic.iso",
      "download.usbBtn": "Образ для USB (.img.xz)",
      "download.releaseBtn": "Сторінка релізу",
      "download.note": "Статус: pre-alpha · Підтримка UEFI (Secure Boot із ARCTIC.cer) та BIOS/CSM · Рекомендовано від 4 ГБ ОЗП.",
      "footer.copy": "Гібридна операційна система для x86-64",
      "footer.docs": "Документація",
      "footer.releases": "Релізи",
      releaseMetaPrefix: "Останній реліз"
    },
    en: {
      pageTitle: "Project Arctic — Next-Generation Hybrid OS for x86",
      "nav.experience": "Experience",
      "nav.performance": "Performance",
      "nav.architecture": "Architecture",
      "nav.download": "Download",
      "nav.cta": "Download ISO",
      "hero.eyebrow": "Next-generation operating system for x86",
      "hero.title": "Familiar comfort.<br/>Next-level speed.",
      "hero.subtitle": "A unique hybrid operating system for x86 combining top-tier performance, native Windows application compatibility, and effortless everyday usability.",
      "hero.download": "Download Live CD (ISO)",
      "hero.github": "View on GitHub",
      "hero.meta": "Latest Live CD release · x86-64 · UEFI Secure Boot & BIOS",
      "hero.modeDark": "Dark mode",
      "hero.modeLight": "Light mode",
      "hero.modeDesktop": "Desktop",
      "common.vmNote": "Real screenshot captured inside a QEMU virtual machine (1920×1080)",
      "metrics.title": "Engineered for peak performance",
      "metrics.subtitle": "The Windows NT subsystem runs directly at the kernel level — without heavy desktop intermediaries.",
      "metrics.gb": "GB",
      "metrics.us": "µs",
      "metrics.ramLabel": "Idle memory footprint",
      "metrics.ramDesc": "Fast cold boot and a lean footprint ready for demanding apps and games.",
      "metrics.win32kLabel": "In-kernel win32k (4.6× faster)",
      "metrics.win32kDesc": "Window creation and messaging executed straight inside the caller thread via kernel syscalls.",
      "metrics.fastpathLabel": "Shared-memory NT registry",
      "metrics.fastpathDesc": "In-process key caching and shared-memory fastpaths eliminate IPC stalls.",
      "metrics.gfxLabel": "Native Vulkan DWM compositor",
      "metrics.gfxDesc": "Direct atomic KMS output, hardware cursor, VRR, and GPU-accelerated Acrylic blur.",
      "showcase.title": "Effortless usability from the first click",
      "showcase.subtitle": "Authentic Windows 11 design standards: rounded geometry, tabbed Explorer, Acrylic glass, and instant response.",
      "showcase.tab1": "Tabbed File Explorer",
      "showcase.tab2": "Multitasking & Snap",
      "showcase.tab3": "Task Manager",
      "showcase.tab4": "Personalization & Themes",
      "showcase.tab1Desc": "Windows 11-style File Explorer with browser tabs, Quick Access, and a real NTFS C: drive.",
      "showcase.tab2Desc": "Clean side-by-side window snapping in Dark mode powered by dwm.exe GPU compositing.",
      "showcase.tab3Desc": "Live telemetry for CPU, memory, physical SSD/HDD disks, network adapters, and GPUs.",
      "showcase.tab4Desc": "Switch seamlessly between Light, Dark, and Custom modes, titlebar blur, and icon packs.",
      "gallery.title": "Light and dark in harmony",
      "gallery.subtitle": "Real VM captures across the system — from PowerShell 7 and system dialogs to the Notification Center.",
      "gallery.card1Title": "PowerShell 7 & About Arctic",
      "gallery.card1Desc": "Native developer shell and system version dialog styled cleanly in Dark mode.",
      "gallery.card2Title": "Notification Center & Calendar",
      "gallery.card2Desc": "Rounded Acrylic flyouts for notifications, calendar, volume, and networking.",
      "gallery.card3Title": "Tailored Personalization",
      "gallery.card3Desc": "Instant switching for system/app theme modes, window title effects, and control styles.",
      "bento.title": "A true hybrid without compromises",
      "bento.subtitle": "One unified Windows NT world on a modern Linux kernel — no X11 or external Wayland desktop.",
      "bento.c1Title": "Hybrid NT + Linux Kernel",
      "bento.c1Desc": "Kernel modules win32k, ntos, ntimage, and ntsync handle Windows syscalls natively. No X11 or third-party Wayland compositor — dwm.exe owns the display and csrss.exe owns input.",
      "bento.c2Title": "Gaming, GPU & KVM",
      "bento.c2Desc": "Direct3D 8–12 via DXVK and vkd3d-proton, Vulkan, CUDA, native WinHvPlatform on KVM, and WSL containers.",
      "bento.c3Title": "Live CD, Live USB & Secure Boot",
      "bento.c3Desc": "Boot straight from the Live CD ISO or persist everything on a real NTFS C: USB partition with Secure Boot and rollback.",
      "download.title": "Experience Project Arctic today",
      "download.subtitle": "Download the latest Live CD image for your virtual machine or modern x86-64 PC.",
      "download.isoBtn": "Download arctic.iso",
      "download.usbBtn": "USB Image (.img.xz)",
      "download.releaseBtn": "Release Notes",
      "download.note": "Status: pre-alpha · UEFI (Secure Boot with ARCTIC.cer) & BIOS/CSM · 4 GB RAM or more recommended.",
      "footer.copy": "Hybrid operating system for x86-64",
      "footer.docs": "Documentation",
      "footer.releases": "Releases",
      releaseMetaPrefix: "Latest release"
    }
  };

  let currentLang = "uk";
  let currentShowcaseKey = "showcase.tab1Desc";
  let releaseMeta = null;

  function detectInitialLang() {
    const params = new URLSearchParams(window.location.search);
    const paramLang = params.get("lang");
    if (paramLang === "en" || paramLang === "uk") return paramLang;
    try {
      const saved = localStorage.getItem("arctic_lang");
      if (saved === "en" || saved === "uk") return saved;
    } catch (_) {}
    return "uk";
  }

  function applyLanguage(lang) {
    currentLang = lang === "en" ? "en" : "uk";
    const dict = I18N[currentLang];
    document.documentElement.lang = currentLang;
    document.title = dict.pageTitle;

    document.querySelectorAll("[data-i18n]").forEach((el) => {
      const key = el.getAttribute("data-i18n");
      if (!dict[key]) return;
      if (key === "hero.title") {
        el.innerHTML = dict[key];
      } else {
        el.textContent = dict[key];
      }
    });

    const showcaseCaption = document.getElementById("showcase-caption");
    if (showcaseCaption && dict[currentShowcaseKey]) {
      showcaseCaption.textContent = dict[currentShowcaseKey];
    }

    document.querySelectorAll(".lang-btn").forEach((btn) => {
      btn.classList.toggle("is-active", btn.dataset.lang === currentLang);
    });

    renderReleaseMetadata();

    try {
      localStorage.setItem("arctic_lang", currentLang);
    } catch (_) {}
  }

  function renderReleaseMetadata() {
    if (!releaseMeta) return;
    const dict = I18N[currentLang];
    document.querySelectorAll(".js-release-name").forEach((el) => {
      el.textContent = releaseMeta.sizeText
        ? `${releaseMeta.name} · arctic.iso (${releaseMeta.sizeText})`
        : `${releaseMeta.name} · arctic.iso`;
    });
    document.querySelectorAll(".js-release-badge").forEach((el) => {
      el.textContent = releaseMeta.sizeText
        ? `${dict.releaseMetaPrefix}: ${releaseMeta.name} (${releaseMeta.sizeText}) · x86-64`
        : `${dict.releaseMetaPrefix}: ${releaseMeta.name} · x86-64`;
    });
  }

  async function fetchLatestRelease() {
    try {
      const res = await fetch("https://api.github.com/repos/77450danik/project-arctic/releases/tags/dev", {
        headers: { Accept: "application/vnd.github+json" }
      });
      if (!res.ok) return;
      const data = await res.json();
      const assets = Array.isArray(data.assets) ? data.assets : [];
      const isoAsset = assets.find((a) => a.name === "arctic.iso");
      const splitIso = assets.find((a) => /^arctic\.iso\.\d{3}$/.test(a.name));
      const usbAsset = assets.find((a) => a.name === "arctic-usb.img.xz");

      const isoUrl = isoAsset ? isoAsset.browser_download_url : (data.html_url || "https://github.com/77450danik/project-arctic/releases/tag/dev");
      document.querySelectorAll(".js-iso-link").forEach((a) => {
        a.href = isoUrl;
      });

      if (usbAsset) {
        document.querySelectorAll(".js-usb-link").forEach((a) => {
          a.href = usbAsset.browser_download_url;
        });
      }

      if (data.html_url) {
        document.querySelectorAll(".js-release-url").forEach((a) => {
          a.href = data.html_url;
        });
      }

      let sizeText = "";
      if (isoAsset && isoAsset.size) {
        const gib = (isoAsset.size / (1024 * 1024 * 1024)).toFixed(2);
        sizeText = `${gib} GiB`;
      } else if (splitIso) {
        sizeText = "split ISO";
      }

      releaseMeta = {
        name: data.name || "Arctic dev",
        sizeText
      };
      renderReleaseMetadata();
    } catch (_) {
      // Keep direct static fallback URLs cleanly intact
    }
  }

  // Language buttons
  document.querySelectorAll(".lang-btn").forEach((btn) => {
    btn.addEventListener("click", () => applyLanguage(btn.dataset.lang));
  });
  document.querySelectorAll(".js-lang-cycle").forEach((btn) => {
    btn.addEventListener("click", () => applyLanguage(currentLang === "uk" ? "en" : "uk"));
  });

  // Hero Light/Dark screenshot pills
  const heroImg = document.getElementById("hero-preview-img");
  document.querySelectorAll("[data-hero-src]").forEach((pill) => {
    pill.addEventListener("click", () => {
      document.querySelectorAll("[data-hero-src]").forEach((p) => p.classList.remove("is-active"));
      pill.classList.add("is-active");
      if (heroImg) heroImg.src = pill.dataset.heroSrc;
    });
  });

  // Interactive showcase tabs
  const showcaseImg = document.getElementById("showcase-img");
  const showcaseCaption = document.getElementById("showcase-caption");
  document.querySelectorAll(".tab-btn").forEach((tab) => {
    tab.addEventListener("click", () => {
      document.querySelectorAll(".tab-btn").forEach((t) => {
        t.classList.remove("is-active");
        t.setAttribute("aria-selected", "false");
      });
      tab.classList.add("is-active");
      tab.setAttribute("aria-selected", "true");
      if (showcaseImg) showcaseImg.src = tab.dataset.img;
      currentShowcaseKey = tab.dataset.captionKey;
      if (showcaseCaption && I18N[currentLang][currentShowcaseKey]) {
        showcaseCaption.textContent = I18N[currentLang][currentShowcaseKey];
      }
    });
  });

  // Lightbox for full-resolution VM screenshot inspection
  const lightbox = document.getElementById("lightbox");
  const lightboxImg = document.getElementById("lightbox-img");
  const lightboxClose = document.getElementById("lightbox-close");

  function closeLightbox() {
    if (!lightbox) return;
    lightbox.hidden = true;
    if (lightboxImg) lightboxImg.src = "";
  }

  document.querySelectorAll(".zoomable").forEach((img) => {
    img.addEventListener("click", () => {
      if (!lightbox || !lightboxImg) return;
      lightboxImg.src = img.currentSrc || img.src;
      lightboxImg.alt = img.alt;
      lightbox.hidden = false;
    });
  });

  if (lightboxClose) lightboxClose.addEventListener("click", closeLightbox);
  if (lightbox) {
    lightbox.addEventListener("click", (e) => {
      if (e.target === lightbox) closeLightbox();
    });
  }
  document.addEventListener("keydown", (e) => {
    if (e.key === "Escape") closeLightbox();
  });

  applyLanguage(detectInitialLang());
  fetchLatestRelease();
})();
