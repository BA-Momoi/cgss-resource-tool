(function () {
  'use strict';

  var manual = document.getElementById('manual');
  if (location.protocol === 'file:') {
    manual.open = true;
    return;
  }

  var library = document.getElementById('library');
  var navigation = document.getElementById('library_navigation');
  var search = document.getElementById('library_search');
  var category = document.getElementById('library_category');
  var refresh = document.getElementById('library_refresh');
  var status = document.getElementById('library_status');
  var list = document.getElementById('library_list');
  var current = document.getElementById('library_current');
  var folder = document.getElementById('library_folder');
  var position = document.getElementById('library_position');
  var previous = document.getElementById('library_previous');
  var next = document.getElementById('library_next');
  var compare = new Intl.Collator('zh-CN', { numeric: true, sensitivity: 'base' }).compare;
  var items = [], visible = [], selectedId = '';
  var selectedEntry = null;
  var defaultsPending = false, rescanTimer;

  library.hidden = false;
  navigation.hidden = false;
  document.getElementById('workspace').classList.add('with-library');

  function updatePosition(){
    var index = visible.findIndex(function(entry){ return entry.id === selectedId; });
    position.textContent = visible.length ? (index < 0 ? '–' : index + 1) + ' / ' + visible.length : '';
    previous.disabled = index <= 0;
    next.disabled = !visible.length || index >= visible.length - 1;
    return index;
  }

  function select(entry){
    selectedEntry = entry;
    selectedId = entry.id;
    current.textContent = entry.title;
    folder.textContent = entry.folder || entry.category;
    Array.from(list.querySelectorAll('button')).forEach(function(button){
      var selected = button.dataset.id === selectedId;
      if (selected) button.setAttribute('aria-current', 'true');
      else button.removeAttribute('aria-current');
    });
    updatePosition();
    loadCatalogEntry(entry);
  }

  function showList(){
    var words = search.value.trim().toLowerCase().split(/\s+/).filter(Boolean);
    visible = items.filter(function(entry){
      return (!category.value || category.value === entry.category) && words.every(function(word){
        return entry.searchText.includes(word);
      });
    });
    var fragment = document.createDocumentFragment();
    var previousCategory = '';
    visible.forEach(function(entry){
      if (previousCategory !== entry.category){
        var header = document.createElement('li');
        header.className = 'library-group';
        header.textContent = entry.category;
        fragment.appendChild(header);
        previousCategory = entry.category;
      }
      var row = document.createElement('li');
      var button = document.createElement('button');
      button.type = 'button';
      button.className = 'resource';
      button.dataset.id = entry.id;
      button.textContent = entry.title;
      button.title = entry.folder || entry.title;
      if (entry.id === selectedId) button.setAttribute('aria-current', 'true');
      button.addEventListener('click', function(){ select(entry); });
      row.appendChild(button);
      fragment.appendChild(row);
    });
    list.replaceChildren(fragment);
    status.textContent = (visible.length ? visible.length + ' 项资源' : items.length ? '没有匹配的资源' : '暂无可预览的 Spine 资源') +
      (defaultsPending ? ' · 正在准备常用动作包…' : '');
    updatePosition();
  }

  function move(delta){
    var index = updatePosition() + delta;
    if (index < 0 || index >= visible.length) return;
    var entry = visible[index];
    select(entry);
    var active = list.querySelector('[aria-current="true"]');
    if (active) active.scrollIntoView({ block: 'nearest' });
  }

  async function scan(){
    clearTimeout(rescanTimer);
    refresh.disabled = true;
    status.textContent = '正在扫描下载目录…';
    try {
      var response = await fetch('/api/catalog.json', { cache: 'no-store' });
      if (!response.ok) throw new Error('HTTP ' + response.status);
      var catalog = await response.json();
      if (!catalog || !Array.isArray(catalog.items)) throw new Error('资源索引格式无效');
      defaultsPending = !!catalog.loadingDefaults;
      items = catalog.items.map(function(entry){
        entry.searchText = [entry.title, entry.category, entry.folder].concat(entry.skeletons.map(function(file){
          return file.name;
        })).join(' ').toLowerCase();
        return entry;
      }).sort(function(a, b){ return compare(a.category, b.category) || compare(a.title, b.title); });
      var previousFilter = category.value;
      category.replaceChildren(new Option('全部分类', ''));
      Array.from(new Set(items.map(function(entry){ return entry.category; }))).forEach(function(name){
        category.appendChild(new Option(name, name));
      });
      if (Array.from(category.options).some(function(option){ return option.value === previousFilter; }))
        category.value = previousFilter;
      showList();
      if (selectedEntry && !selectedEntry.atlas && !atlasText && !document.getElementById('skin_apply').disabled){
        var completedEntry = items.find(function(entry){ return entry.id === selectedId && entry.atlas; });
        if (completedEntry) select(completedEntry);
      } else if (!selectedId && visible.length && !previewReady) select(visible[0]);
    } catch (err) {
      status.textContent = '资源目录读取失败：' + err.message;
      if (!items.length) manual.open = true;
    } finally {
      refresh.disabled = false;
      if (defaultsPending) rescanTimer = setTimeout(scan, 2000);
    }
  }

  search.addEventListener('input', showList);
  category.addEventListener('change', showList);
  refresh.addEventListener('click', scan);
  previous.addEventListener('click', function(){ move(-1); });
  next.addEventListener('click', function(){ move(1); });
  updatePosition();
  scan();
})();
