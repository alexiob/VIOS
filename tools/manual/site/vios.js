/* VIOS manual: style switch, table scrolling, contents on phones, word filter. */
(function () {
  var root = document.documentElement;

  /* Smalltalk / Turbo Vision, like 0 UI-STYLE and 1 UI-STYLE. */
  var button = document.getElementById('style');
  if (button) button.addEventListener('click', function () {
    var next = root.dataset.style === 'turbo' ? 'smalltalk' : 'turbo';
    root.dataset.style = next;
    try { localStorage.setItem('vios-style', next); } catch (e) {}
  });

  /* Copy to clipboard on every code block. The icon is drawn on the 8x8 grid
   * of the Agon font: a clipboard, and a tick once copied. */
  var CLIP = 'M2 1h4v1h1v6H1V2h1zM3 0h2v1H3zM2 3v4h4V3z';
  var TICK = 'M6 1h1v2H6v1H5v1H4v1H3v1H2V6H1V4h1v1h1V4h1V3h1V2h1z';
  var icon = function (d) { return '<svg viewBox="0 0 8 8" aria-hidden="true"><path d="' + d + '"/></svg>'; };
  var legacyCopy = function (text) {
    return new Promise(function (ok, fail) {
      var t = document.createElement('textarea');
      t.value = text; t.setAttribute('readonly', ''); t.style.position = 'fixed'; t.style.opacity = '0';
      document.body.appendChild(t); t.select();
      try { document.execCommand('copy') ? ok() : fail(); } catch (e) { fail(e); }
      document.body.removeChild(t);
    });
  };
  var copyText = function (text) {
    if (navigator.clipboard && window.isSecureContext)
      return navigator.clipboard.writeText(text).catch(function () { return legacyCopy(text); });
    return legacyCopy(text);
  };
  document.querySelectorAll('article pre').forEach(function (pre) {
    var code = pre.querySelector('code') || pre;
    var b = document.createElement('button');
    b.type = 'button';
    b.className = 'copy';
    b.title = 'Copy to clipboard';
    b.setAttribute('aria-label', 'Copy code to clipboard');
    b.innerHTML = icon(CLIP) + '<span>Copy</span>';
    b.addEventListener('click', function () {
      copyText(code.innerText.replace(/\n$/, '')).then(function () {
        b.classList.add('done');
        b.innerHTML = icon(TICK) + '<span>Copied</span>';
        setTimeout(function () { b.classList.remove('done'); b.innerHTML = icon(CLIP) + '<span>Copy</span>'; }, 1500);
      }, function () { b.querySelector('span').textContent = 'Select and copy'; });
    });
    pre.appendChild(b);
  });

  /* The menu bar: show which side has more items when it scrolls. */
  var bar = document.querySelector('.menubar');
  if (bar) {
    var edges = function () {
      var max = bar.scrollWidth - bar.clientWidth;
      bar.classList.toggle('more-left', bar.scrollLeft > 2);
      bar.classList.toggle('more-right', max > 2 && bar.scrollLeft < max - 2);
    };
    bar.addEventListener('scroll', edges, { passive: true });
    window.addEventListener('resize', edges);
    edges();
  }

  /* Wide tables scroll inside their own frame instead of the page. */
  document.querySelectorAll('article table').forEach(function (t) {
    var wrap = document.createElement('div');
    wrap.className = 'table-wrap';
    t.parentNode.insertBefore(wrap, t);
    wrap.appendChild(t);
  });

  /* The contents window starts closed on narrow screens. */
  var toc = document.getElementById('toc');
  if (toc && window.matchMedia('(max-width: 1100px)').matches) toc.open = false;

  /* Word reference: filter rows by word, section or description. */
  var input = document.getElementById('wf');
  if (input) {
    var rows = Array.prototype.slice.call(document.querySelectorAll('article tbody tr'));
    var count = document.createElement('span');
    count.className = 'count';
    input.parentNode.appendChild(count);
    var sections = Array.prototype.slice.call(document.querySelectorAll('article h3'));
    var apply = function () {
      var q = input.value.trim().toLowerCase(), shown = 0;
      rows.forEach(function (r) {
        var cells = r.cells, name = cells[0] ? cells[0].textContent.toLowerCase() : '';
        var hit = !q || name.indexOf(q) >= 0 || (q.length > 2 && r.textContent.toLowerCase().indexOf(q) >= 0);
        r.hidden = !hit;
        if (hit) shown++;
      });
      sections.forEach(function (h) {
        var wrap = h.nextElementSibling;
        while (wrap && !wrap.classList.contains('table-wrap') && wrap.tagName !== 'H3') wrap = wrap.nextElementSibling;
        if (!wrap || !wrap.classList.contains('table-wrap')) return;
        var any = wrap.querySelector('tbody tr:not([hidden])');
        var hide = q && !any;
        h.hidden = hide;
        wrap.hidden = hide;
        var p = h.nextElementSibling;
        if (p && p.tagName === 'P') p.hidden = hide;
      });
      count.textContent = q ? shown + ' of ' + rows.length + ' words' : rows.length + ' words';
    };
    input.addEventListener('input', apply);
    apply();
  }
})();
