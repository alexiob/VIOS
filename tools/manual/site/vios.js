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
