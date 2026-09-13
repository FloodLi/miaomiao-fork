// Legacy ID37 / other-panel UI regression. ID34 is covered by editor_regression.js.
// Run with: playwright-cli -s=jd-review run-code --filename test/web/jd_image_regression.js
// Requires the production web server at 127.0.0.1:8765. No device requests are sent.
// Writes output/playwright/jd-image-regression-report.json and jd-*.png diagnostics.
async (page) => {
  const origin = 'http://127.0.0.1:8765';
  const report = {startedAt: new Date().toISOString(), reportPath: 'output/playwright/jd-image-regression-report.json', checks: [], panels: [], layouts: [], failures: [], pageErrors: [], blockedRequests: []};
  const check = (name, pass, metrics) => {
    report.checks.push({name, pass, ...metrics});
    if (!pass) report.failures.push(name);
  };
  const panelSizes = {34: [800, 480], 37: [400, 300], 26: [400, 300], 44: [960, 672], 43: [792, 528]};
  let activePanel = 34;
  const onError = error => report.pageErrors.push(String(error));
  page.on('pageerror', onError);
  const intercept = async route => {
    const request = route.request();
    const pathname = request.url().slice(origin.length).split('?')[0];
    if (!request.url().startsWith(origin + '/') || request.method() !== 'GET') {
      report.blockedRequests.push({method: request.method(), url: request.url()});
      return route.abort();
    }
    if (pathname === '/status') return route.fulfill({json: {
      panel: activePanel, panel_width: panelSizes[activePanel][0], panel_height: panelSizes[activePanel][1],
      panel_has_red: activePanel !== 26, panel_has_yellow: activePanel !== 26, panel_name: 'Browser regression ' + activePanel,
      raw_bytes: 0, spiffs_ok: true, spiffs_total: 1048576, spiffs_used: 0,
      version: 'browser-test', time_synced: false, epd_ready: false, battery_valid: false
    }});
    if (pathname === '/images') return route.fulfill({json: {images: []}});
    if (pathname === '/wifi_status') return route.fulfill({json: {mode: 3, sta_connected: false}});
    if (/\.(html|js|css|ico|png|svg|woff2?)$/.test(pathname) || pathname === '/') return route.continue();
    report.blockedRequests.push({method: request.method(), url: request.url()});
    return route.abort();
  };
  await page.route('**/*', intercept);
  if (!page.url().startsWith(origin)) await page.goto(origin + '/index.html');
  const savedStorage = await page.evaluate(() => Object.fromEntries(Object.entries(localStorage)));
  let diagnostics;
  try {
    await page.evaluate(() => {
      Object.keys(localStorage).filter(k => k.startsWith('upload_img_opts')).forEach(k => localStorage.removeItem(k));
      localStorage.setItem('epd_onboard_done', '1');
    });
    await page.goto(origin + '/index.html');
    await page.evaluate(async () => { await refresh(); });
    await page.setViewportSize({width: 1440, height: 900});
    diagnostics = await page.context().newPage();
    await diagnostics.setViewportSize({width: 1440, height: 900});
    const savePng = async (name, dataUrl, width, height) => {
      await diagnostics.setContent('<style>body{margin:0}img{display:block}</style><img id="artifact">');
      await diagnostics.locator('#artifact').evaluate(async (img, data) => {
        img.src = data;
        await img.decode();
      }, dataUrl);
      await diagnostics.locator('#artifact').screenshot({path: 'output/playwright/jd-' + name + '.png', scale: 'css'});
      check(name + ' native artifact dimensions', await diagnostics.locator('#artifact').evaluate(img => img.naturalWidth === img.width && img.naturalHeight === img.height), {width, height});
    };
    await page.evaluate(() => {
      // Fixtures and measurements only: quantization stays in the production functions.
      window.jdRegression = {
        canvas(w, h) { const c = document.createElement('canvas'); c.width = w; c.height = h; return c; },
        pixels(c) { return c.getContext('2d').getImageData(0, 0, c.width, c.height).data; },
        source(w, h) {
          const c = this.canvas(w, h), ctx = c.getContext('2d');
          ctx.fillStyle = '#fff'; ctx.fillRect(0, 0, w, h);
          const ramp = {x: 1, y: 1, w: w - 2, h: Math.floor(h * .25)};
          for (let x = 0; x < ramp.w; x++) {
            const v = Math.round(x * 255 / (ramp.w - 1));
            ctx.fillStyle = `rgb(${v},${v},${v})`; ctx.fillRect(x + 1, 1, 1, ramp.h);
          }
          const patches = [];
          for (let i = 0; i < 17; i++) {
            const x = 1 + Math.floor((w - 2) * i / 17), right = 1 + Math.floor((w - 2) * (i + 1) / 17);
            const v = Math.round(i * 255 / 16), y = ramp.h + 1, ph = Math.floor(h * .25);
            ctx.fillStyle = `rgb(${v},${v},${v})`; ctx.fillRect(x, y, right - x, ph);
            patches.push({x, y, w: right - x, h: ph, v});
          }
          const y = patches[0].y + patches[0].h, sh = Math.floor(h * .24), swatches = [];
          ['#ff0000', '#808080', '#ffff00', '#c0c0c0'].forEach((color, i) => {
            const x = 1 + Math.floor((w - 2) * i / 4), right = 1 + Math.floor((w - 2) * (i + 1) / 4);
            ctx.fillStyle = color; ctx.fillRect(x, y, right - x, sh);
            swatches.push({x, y, w: right - x, h: sh, color});
          });
          ctx.fillStyle = '#000'; ctx.font = 'bold ' + Math.floor(h * .06) + 'px sans-serif';
          ctx.fillText('JD ' + w + 'x' + h + '  Aa 012345', 8, h - 30);
          ctx.font = '12px monospace'; ctx.fillText('1px border / gray ramp / red + yellow', 8, h - 10);
          ctx.fillRect(0, 0, w, 1); ctx.fillRect(0, h - 1, w, 1);
          ctx.fillRect(0, 0, 1, h); ctx.fillRect(w - 1, 0, 1, h);
          return {canvas: c, patches, swatches};
        },
        async useSource(c) {
          const img = new Image(); img.src = c.toDataURL(); await img.decode();
          previewImg = img; document.getElementById('previewWrap').style.display = 'grid'; renderPreview();
        },
        async exportMetrics() {
          renderPreview();
          const preview = document.getElementById('epdPreview'), a = this.pixels(preview);
          const blob = await buildUploadBlobFromPreview(), bitmap = await createImageBitmap(blob);
          const c = this.canvas(bitmap.width, bitmap.height); c.getContext('2d').drawImage(bitmap, 0, 0); bitmap.close();
          const b = this.pixels(c); let differingPixels = 0;
          for (let i = 0; i < a.length; i += 4) if (a[i] !== b[i] || a[i + 1] !== b[i + 1] || a[i + 2] !== b[i + 2] || a[i + 3] !== b[i + 3]) differingPixels++;
          return {differingPixels, width: c.width, height: c.height, mime: blob.type, dataUrl: c.toDataURL()};
        },
        measure(c, fixture) {
          const d = this.pixels(c), source = this.pixels(fixture.canvas), w = c.width;
          const palette = new Set(); let neutralColored = 0, endpointChanges = 0;
          for (let i = 0; i < d.length; i += 4) {
            palette.add([d[i], d[i + 1], d[i + 2], d[i + 3]].join(','));
            const neutral = source[i] === source[i + 1] && source[i] === source[i + 2];
            if (neutral && (d[i] !== d[i + 1] || d[i] !== d[i + 2])) neutralColored++;
            if (neutral && (source[i] === 0 || source[i] === 255) && (d[i] !== source[i] || d[i + 1] !== source[i] || d[i + 2] !== source[i])) endpointChanges++;
          }
          const patches = fixture.patches.map(p => {
            let sum = 0, n = 0;
            for (let y = p.y + 2; y < p.y + p.h - 2; y++) for (let x = p.x + 2; x < p.x + p.w - 2; x++) { sum += d[(y * w + x) * 4]; n++; }
            return {input: p.v, mean: +(sum / n).toFixed(3), error: +(sum / n - p.v).toFixed(3)};
          });
          const swatches = fixture.swatches.map(p => {
            const counts = {};
            for (let y = p.y; y < p.y + p.h; y++) for (let x = p.x; x < p.x + p.w; x++) {
              const i = (y * w + x) * 4, key = [d[i], d[i + 1], d[i + 2]].join(','); counts[key] = (counts[key] || 0) + 1;
            }
            return {source: p.color, counts, total: p.w * p.h};
          });
          return {palette: [...palette], neutralColored, endpointChanges, patches, swatches};
        }
      };
    });
    const selectPanel = async id => { activePanel = id; await page.evaluate(async () => { await refresh(); }); };
    const selectPreset = async preset => {
      // ID34's retired named preset remains an internal compatibility path.
      if (activePanel === 34 && preset === 'photo') await page.evaluate(() => { applyPreset('photo', false); renderPreview(); });
      else await page.locator('#presetSeg [data-preset="' + preset + '"]').click();
    };
    const assertExport = (name, result, w, h) => check(name + ' preview/export equality', result.differingPixels === 0 && result.width === w && result.height === h && result.mime === 'image/png', {differingPixels: result.differingPixels, width: result.width, height: result.height});
    for (const id of [37]) {
      await selectPanel(id);
      const [w, h] = panelSizes[id];
      const source = await page.evaluate(async ({w, h}) => {
        jdRegression.fixture = jdRegression.source(w, h);
        await jdRegression.useSource(jdRegression.fixture.canvas);
        return jdRegression.fixture.canvas.toDataURL();
      }, {w, h});
      await savePng(id + '-source', source, w, h);
      const comparison = [{label: 'Source', dataUrl: source}];
      await page.locator('#f').setInputFiles('output/playwright/jd-' + id + '-source.png');
      await page.waitForFunction(({w, h}) => previewImg && previewImg.width === w && previewImg.height === h && document.getElementById('previewWrap').style.display === 'grid', {w, h});
      const panel = {id, width: w, height: h, presets: []}; report.panels.push(panel);
      for (const preset of ['photo', 'gray', 'text']) {
        await selectPreset(preset);
        const result = await page.evaluate(async () => ({...await jdRegression.exportMetrics(), ...jdRegression.measure(document.getElementById('epdPreview'), jdRegression.fixture), options: previewOpts()}));
        const {dataUrl, ...metrics} = result; panel.presets.push({preset, ...metrics});
        assertExport(id + ' ' + preset, result, w, h);
        check(id + ' ' + preset + ' neutral safeguard', result.neutralColored === 0, {neutralColored: result.neutralColored});
        check(id + ' ' + preset + ' exact black/white', result.endpointChanges === 0, {endpointChanges: result.endpointChanges});
        if (preset !== 'text') check(id + ' ' + preset + ' monotonic density', result.patches.every((p, i, a) => !i || p.mean >= a[i - 1].mean), {means: result.patches.map(p => p.mean)});
        if (preset === 'gray') check(id + ' gray density tolerance', result.patches.every(p => Math.abs(p.error) <= 6), {maxError: Math.max(...result.patches.map(p => Math.abs(p.error)))});
        if (preset !== 'photo') check(id + ' ' + preset + ' monochrome palette', result.palette.every(p => ['0,0,0,255', '255,255,255,255'].includes(p)), {palette: result.palette});
        if (preset === 'photo') check(id + ' saturated red/yellow preserved', result.swatches[0].counts['192,57,43'] === result.swatches[0].total && result.swatches[2].counts['210,165,24'] === result.swatches[2].total, {swatches: result.swatches});
        if (preset === 'text') {
          const grain = await page.evaluate(() => {
            const c = document.getElementById('epdPreview'), d = jdRegression.pixels(c);
            return jdRegression.fixture.patches.map(p => {
              const values = new Set();
              for (let y = p.y + 2; y < p.y + p.h - 2; y++) for (let x = p.x + 2; x < p.x + p.w - 2; x++) values.add(d[(y * c.width + x) * 4]);
              return values.size;
            });
          });
          check(id + ' text solid fills without grain', grain.every(n => n === 1), {uniqueValuesPerPatch: grain});
        }
        await savePng(id + '-' + (preset === 'photo' ? 'color' : preset), dataUrl, w, h);
        if (preset !== 'text') comparison.push({label: preset === 'photo' ? 'Color photo' : 'Gray photo', dataUrl});
      }
      await diagnostics.setViewportSize({width: w + 32, height: h * 3 + 150});
      await diagnostics.setContent('<style>body{margin:16px;font:14px sans-serif;background:#eee}p{margin:12px 0 4px}img{display:block}</style><main id="comparison"></main>');
      await diagnostics.locator('#comparison').evaluate(async (root, entries) => {
        for (const entry of entries) {
          const label = document.createElement('p'); label.textContent = entry.label;
          const img = new Image(); img.src = entry.dataUrl; await img.decode(); root.append(label, img);
        }
      }, comparison);
      await diagnostics.locator('#comparison').screenshot({path: 'output/playwright/jd-' + id + '-comparison.png', scale: 'css'});
      await diagnostics.setViewportSize({width: 1440, height: 900});
      await selectPreset('photo');
      const strong = await page.evaluate(async () => {
        for (const [id, value] of Object.entries({redMode: 'strong', sharp: '100', redSens: '100', yellowSens: '100'})) {
          const el = document.getElementById(id); el.value = value; el.dispatchEvent(new Event('input', {bubbles: true}));
        }
        return {...await jdRegression.exportMetrics(), ...jdRegression.measure(document.getElementById('epdPreview'), jdRegression.fixture)};
      });
      assertExport(id + ' strong red/max sharp', strong, w, h);
      check(id + ' strong red/max sharp neutral safeguard', strong.neutralColored === 0, {neutralColored: strong.neutralColored});
      check(id + ' strong saturated inks', strong.swatches[0].counts['192,57,43'] === strong.swatches[0].total && strong.swatches[2].counts['210,165,24'] === strong.swatches[2].total, {});
      await savePng(id + '-strong', strong.dataUrl, w, h);
      const boundary = await page.evaluate(() => {
        const w = epdPanelW, h = epdPanelH, opt = {...previewOpts(), contrast: 1, brightness: 0, gamma: 1, sharpness: 1, redMode: 'strong', palette: 'color'};
        const c = jdRegression.canvas(w, h), ctx = c.getContext('2d'), im = ctx.createImageData(w, h);
        for (let i = 0; i < im.data.length; i += 4) {
          const x = (i / 4) % w;
          if (x < w / 3) { im.data[i] = 255; im.data[i + 1] = 0; im.data[i + 2] = 0; im.data[i + 3] = 0; }
          else { im.data[i] = 138; im.data[i + 1] = 128; im.data[i + 2] = 118; im.data[i + 3] = 255; }
        }
        ctx.putImageData(im, 0, 0); ditherPreview(c, opt);
        const data = jdRegression.pixels(c); let alphaWhiteChanges = 0, nearNeutralColored = 0;
        for (let i = 0; i < data.length; i += 4) {
          if ((i / 4) % w < w / 3 && (data[i] !== 255 || data[i + 1] !== 255 || data[i + 2] !== 255 || data[i + 3] !== 255)) alphaWhiteChanges++;
          if ((i / 4) % w >= w / 3 && (data[i] !== data[i + 1] || data[i] !== data[i + 2])) nearNeutralColored++;
        }
        const a = jdRegression.canvas(w, h), b = jdRegression.canvas(w, h);
        for (const canvas of [a, b]) { const x = canvas.getContext('2d'); x.fillStyle = '#fff'; x.fillRect(0, 0, w, h); }
        for (let x = 0; x < w; x++) {
          const v = 24 + Math.round(x * 207 / (w - 1));
          a.getContext('2d').fillStyle = b.getContext('2d').fillStyle = `rgb(${v},${v},${v})`;
          a.getContext('2d').fillRect(x, 0, 1, 1); b.getContext('2d').fillRect(w - x - 1, 1, 1, 1);
        }
        const rowOpt = {...opt, sharpness: 0, dither: 1, threshold: 128};
        ditherPreview(a, rowOpt); ditherPreview(b, rowOpt);
        const ap = jdRegression.pixels(a), bp = jdRegression.pixels(b); let mirroredRowMismatch = 0;
        for (let x = 0; x < w; x++) if (ap[x * 4] !== bp[(2 * w - x - 1) * 4]) mirroredRowMismatch++;
        return {alphaWhiteChanges, nearNeutralColored, mirroredRowMismatch};
      });
      check(id + ' transparent source composites white', boundary.alphaWhiteChanges === 0, boundary);
      check(id + ' source chroma20 stays neutral', boundary.nearNeutralColored === 0, boundary);
      check(id + ' serpentine mirrored odd row', boundary.mirroredRowMismatch === 0, boundary);
      await page.locator('#presetSeg [data-preset="gray"]').click();
      const custom = await page.evaluate(async () => {
        for (const [id, value] of Object.entries({brightness: '7', contrast: '123', gamma: '112', sharp: '80', redMode: 'strong', redSens: '100', yellowSens: '100', dither: '83'})) {
          const el = document.getElementById(id); el.value = value; el.dispatchEvent(new Event('input', {bubbles: true}));
        }
        const before = previewOpts(); saveUploadPreviewOptions(); applyPreset('photo', false); loadUploadPreviewOptions();
        return {before, after: previewOpts(), ...await jdRegression.exportMetrics(), ...jdRegression.measure(document.getElementById('epdPreview'), jdRegression.fixture)};
      });
      assertExport(id + ' edited mono roundtrip', custom, w, h);
      check(id + ' edited gray palette/storage', custom.before.preset === 'custom' && JSON.stringify(custom.before) === JSON.stringify(custom.after) && custom.palette.every(p => ['0,0,0,255', '255,255,255,255'].includes(p)), {before: custom.before, after: custom.after, palette: custom.palette});
      // Exercise the visible palette selector independently of the gray preset.
      await selectPreset('photo');
      await page.locator('#jdPalette').selectOption('mono');
      const mono = await page.evaluate(async () => { loadUploadPreviewOptions(); return {...await jdRegression.exportMetrics(), ...jdRegression.measure(document.getElementById('epdPreview'), jdRegression.fixture)}; });
      check(id + ' palette selector mono roundtrip', mono.palette.every(p => ['0,0,0,255', '255,255,255,255'].includes(p)), {});
      assertExport(id + ' palette selector', mono, w, h);
      for (const [width, height] of [[1440, 900], [390, 844], [360, 800]]) {
        await page.setViewportSize({width, height});
        await page.locator('#imageMoreSettings').evaluate(el => { el.open = true; });
        const layout = await page.evaluate(() => {
          const visible = el => { const r = el.getBoundingClientRect(); return r.width > 0 && r.height > 0 && getComputedStyle(el).visibility !== 'hidden'; };
          const controls = [...document.querySelectorAll('#previewWrap input,#previewWrap select,#previewWrap button,#previewWrap summary')].filter(visible);
          const outside = controls.filter(el => { const r = el.getBoundingClientRect(); return r.left < -1 || r.right > innerWidth + 1; }).map(el => el.id || el.textContent);
          const clipped = controls.filter(el => el.tagName === 'BUTTON' && el.scrollWidth > el.clientWidth + 1).map(el => el.textContent);
          const presets = [...document.querySelectorAll('#presetSeg button')].filter(visible);
          const shortPresets = presets.filter(el => el.getBoundingClientRect().height < 44).map(el => el.dataset.preset);
          const wrappedPresets = presets.filter(el => getComputedStyle(el).whiteSpace !== 'nowrap').map(el => el.dataset.preset);
          return {scrollWidth: document.documentElement.scrollWidth, viewport: innerWidth, outside, clipped, shortPresets, wrappedPresets, visibleControls: controls.length, grayVisible: visible(document.querySelector('[data-preset="gray"]')), e6Visible: visible(document.querySelector('[data-preset="e6fs"]')), paletteVisible: visible(document.getElementById('jdPalette'))};
        });
        report.layouts.push({id, width, height, ...layout});
        check(id + ' layout ' + width, layout.scrollWidth <= width && !layout.outside.length && !layout.clipped.length && !layout.shortPresets.length && !layout.wrappedPresets.length && !layout.e6Visible && layout.visibleControls > 20 && layout.grayVisible && layout.paletteVisible, layout);
        await page.screenshot({path: 'output/playwright/jd-' + id + '-ui-' + width + 'x' + height + '.png', fullPage: true});
      }
      await page.setViewportSize({width: 1440, height: 900});
    }
    for (const id of [37]) {
      await selectPanel(id);
      const migration = await page.evaluate(() => {
        const fields = {threshold: 'thr', brightness: 'brightness', contrast: 'contrast', gamma: 'gamma', sharp: 'sharp', red: 'redSens', yellow: 'yellowSens', dither: 'dither', redMode: 'redMode'};
        const legacy = {threshold: 96, brightness: -7, contrast: 120, gamma: 92, sharp: 30, red: 23, yellow: 19, dither: 70, redMode: 'safe'};
        const result = [];
        for (const version of [undefined, 1]) for (const preset of ['photo', 'anime', 'text', 'soft', 'gray', 'e6fs']) {
          localStorage.setItem(uploadOptionsStorageKey(), JSON.stringify({...legacy, preset, quantVersion: version, e6FsMode: preset === 'e6fs'}));
          loadUploadPreviewOptions();
          const fallback = epdPanelId === 34 ? 'colorphoto' : 'photo';
          const expected = uploadPresetValues()[preset === 'e6fs' || (epdPanelId === 34 && preset === 'photo') ? fallback : preset], mismatch = Object.entries(fields).filter(([key, id]) => String(expected[key]) !== document.getElementById(id).value).map(([key]) => key);
          if (preset === 'e6fs' && (previewPreset !== fallback || previewE6FsMode)) mismatch.push('e6fs fallback');
          result.push({preset, version: version || 'absent', mismatch, options: previewOpts()});
        }
        localStorage.setItem(uploadOptionsStorageKey(), JSON.stringify({...legacy, preset: 'custom', palette: 'mono'}));
        loadUploadPreviewOptions();
        const customMismatch = Object.entries(fields).filter(([key, id]) => String(legacy[key]) !== document.getElementById(id).value).map(([key]) => key);
        const custom = previewOpts();
        const oldPureBw = {...legacy, preset: 'custom', dither: 0, redMode: 'off'};
        localStorage.setItem(uploadOptionsStorageKey(), JSON.stringify(oldPureBw));
        loadUploadPreviewOptions();
        const oldPureBwMismatch = Object.entries(fields).filter(([key, id]) => String(oldPureBw[key]) !== document.getElementById(id).value).map(([key]) => key);
        renderPreview();
        const d = jdRegression.pixels(document.getElementById('epdPreview'));
        const oldPureBwPalette = [...new Set(Array.from({length: epdPanelW * epdPanelH}, (_, i) => {
          return [d[i * 4], d[i * 4 + 1], d[i * 4 + 2]].join(',');
        }))];
        return {named: result, customMismatch, custom, oldPureBw: previewOpts(), oldPureBwMismatch, oldPureBwPalette};
      });
      check(id + ' legacy named preset migration', migration.named.every(x => !x.mismatch.length), migration);
      check(id + ' legacy custom numeric preservation', !migration.customMismatch.length && migration.custom.preset === 'custom' && migration.custom.palette === 'mono', {custom: migration.custom, mismatch: migration.customMismatch});
      check(id + ' legacy custom pure BW compatibility', !migration.oldPureBwMismatch.length && migration.oldPureBw.palette === 'mono' && migration.oldPureBwPalette.every(p => p === '0,0,0' || p === '255,255,255'), {options: migration.oldPureBw, mismatch: migration.oldPureBwMismatch, palette: migration.oldPureBwPalette});
    }
    const customByPanel = {};
    for (const id of [37]) {
      await selectPanel(id);
      customByPanel[id] = await page.evaluate(id => {
        const el = document.getElementById('brightness'); el.value = id === 34 ? '-13' : '17'; el.dispatchEvent(new Event('input', {bubbles: true})); return previewOpts();
      }, id);
    }
    for (const id of [37]) {
      await selectPanel(id);
      check(id + ' independent custom settings', JSON.stringify(await page.evaluate(() => previewOpts())) === JSON.stringify(customByPanel[id]), {expected: customByPanel[id], actual: await page.evaluate(() => previewOpts())});
    }
    // Spy on dispatch, never replace or copy a production quantizer.
    for (const id of [26, 44, 43]) {
      await selectPanel(id);
      const isolation = await page.evaluate(async () => {
        const original = ditherPreviewJd; let calls = 0;
        ditherPreviewJd = (...args) => { calls++; return original(...args); };
        try {
          const exported = await jdRegression.exportMetrics();
          const visible = el => { const r = el.getBoundingClientRect(); return r.width > 0 && r.height > 0; };
          return {calls, differingPixels: exported.differingPixels, grayVisible: visible(document.querySelector('[data-preset="gray"]')), paletteVisible: visible(document.getElementById('jdPaletteCtrl'))};
        } finally { ditherPreviewJd = original; }
      });
      check(id + ' non-JD routing and hidden controls', isolation.calls === 0 && !isolation.grayVisible && !isolation.paletteVisible && isolation.differingPixels === 0, isolation);
    }
    // Keep the final E6 migration/gating checks in the same repeatable run.
    for (const id of [37, 43]) {
      await selectPanel(id);
      const e6Checks = await page.evaluate(() => {
        const results = [], key = uploadOptionsStorageKey(), saved = localStorage.getItem(key);
        const jd = isJdFourColorPreviewPanel();
        try {
          for (const version of [undefined, 1]) {
            localStorage.setItem(key, JSON.stringify({preset: 'e6fs', e6FsMode: true, quantVersion: version, threshold: 96, contrast: 120, dither: 70}));
            loadUploadPreviewOptions();
            const fallback = epdPanelId === 34 ? 'colorphoto' : 'photo';
            const values = uploadPresetValues()[fallback], options = previewOpts();
            const pass = previewPreset === (jd ? fallback : 'e6fs') && previewE6FsMode === !jd &&
              (!jd || (options.threshold === values.threshold && options.contrast === values.contrast / 100 && options.dither === values.dither / 100));
            results.push({case: 'cached E6 flag version ' + (version || 'absent'), pass, preset: previewPreset, e6FsMode: previewE6FsMode, options});
          }
          applyPreset('e6fs', false);
          results.push({case: 'applyPreset E6 gating', pass: previewE6FsMode === !jd, preset: previewPreset, e6FsMode: previewE6FsMode});
        } finally {
          if (saved === null) localStorage.removeItem(key);
          else localStorage.setItem(key, saved);
          loadUploadPreviewOptions();
        }
        return results;
      });
      for (const result of e6Checks) {
        const {pass, ...metrics} = result;
        check(id + ' ' + result.case, pass, metrics);
      }
    }
    check('no browser exceptions', !report.pageErrors.length, {errors: report.pageErrors});
    check('no upload or device actions attempted', !report.blockedRequests.length, {requests: report.blockedRequests});
  } finally {
    if (diagnostics) await diagnostics.close();
    await page.evaluate(saved => {
      Object.keys(localStorage).filter(k => k.startsWith('upload_img_opts') || k === 'epd_onboard_done').forEach(k => localStorage.removeItem(k));
      Object.entries(saved).filter(([k]) => k.startsWith('upload_img_opts') || k === 'epd_onboard_done').forEach(([k, v]) => localStorage.setItem(k, v));
      delete window.jdRegression;
    }, savedStorage);
    await page.unroute('**/*', intercept);
    page.removeListener('pageerror', onError);
  }
  report.passed = report.checks.filter(x => x.pass).length;
  report.finishedAt = new Date().toISOString();
  const reportDownload = page.waitForEvent('download');
  await page.evaluate(result => {
    const url = URL.createObjectURL(new Blob([JSON.stringify(result, null, 2)], {type: 'application/json'}));
    const link = document.createElement('a');
    link.href = url; link.download = 'jd-image-regression-report.json';
    document.body.appendChild(link); link.click(); link.remove();
    setTimeout(() => URL.revokeObjectURL(url), 1000);
  }, report);
  await (await reportDownload).saveAs(report.reportPath);
  if (report.failures.length) throw new Error(JSON.stringify(report, null, 2));
  return report;
}
