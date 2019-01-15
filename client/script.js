var dbname = window.location.hash.slice(1);

var initialState = {
  children: [],
  type: 'initial',
};

initialState.children.push({
  parent: initialState,
  query: { code: '' },
  type: 'input',
});

var root = initialState;

function query(req, cb) {
  $.ajax({
    url: '/eval/' + dbname,
    type: 'POST',
    data: JSON.stringify(req),
    contentType: 'application/json',
    success: cb,
  });
}

var head = null;

render();

function renderGens(gen, pnode) {
  var entry = $('<div class="entry"></div>');

  if (gen.type === 'initial') {
    entry.addClass('initial-msg');
    entry.text('initial state');
  } else if (gen.type === 'input') {
    var tbox = $('<textarea class="query-code" rows="2"></textarea>');
    if (gen.query && gen.query.code) {
      tbox.text(gen.query.code);
    }
    entry
      .addClass('query')
      .append(tbox)
      .append($('<button class="go-query">&nbsp;go&nbsp;</button>'))
      .data('gen', gen);

    if (head === gen.parent || gen.gen === undefined) {
      entry.addClass('athead');
    }
  } else if (gen.type === 'result') {
    var result = gen.result;
    var error = result.error;
    var object = result.object;

    entry.append(
      $('<div class="query readonly"></div>')
        .append(
          $(
            '<textarea class="query-code" readonly="readonly"></textarea>',
          ).text(gen.query.code),
        )
        .append($('<button class="edit-query">edit</button>').data('gen', gen)),
    );

    entry.append(
      $('<div class="status-line"></div>').text(
        'gen: ' +
          result.gen +
          ' | ' +
          'walltime: ' +
          result.walltime / 1e6 +
          'ms' +
          ' | ' +
          (result.warm ? 'warm' : 'cold'),
      ),
    );

    if (error) {
      entry.addClass('error');
      entry.append(
        $('<div class="result error"></div>').text(
          JSON.stringify(error, null, 2),
        ),
      );
    } else {
      entry.append(
        $('<div class="result"></div>').text(JSON.stringify(object, null, 2)),
      );
    }
  }

  pnode.append(entry);

  if (gen.children) {
    for (var i = 0; i < gen.children.length; i++) {
      if (i !== gen.children.length - 1) {
        var fork = $('<div class="timeline fork"></div>');
        pnode.append(fork);
        renderGens(gen.children[i], fork);
      } else {
        renderGens(gen.children[i], pnode);
      }
    }
  }
}

function render() {
  $('#root-timeline').empty();
  renderGens(root, $('#root-timeline'));
}

function handleSubmit(entry) {
  var q = entry.children('.query-code');
  var inputEntry = entry.data('gen');

  var req = {
    code: q.val(),
    gen: inputEntry === head ? undefined : inputEntry.gen,
  };

  query(req, function(result) {
    inputEntry.query = req;
    inputEntry.result = result;
    inputEntry.children = [];
    inputEntry.type = 'result';

    if (result.error) {
      inputEntry.parent.children.push({
        type: 'input',
        parent: inputEntry.parent,
        gen: inputEntry.gen,
      });
      head = inputEntry.parent;
    } else {
      inputEntry.children.push({
        type: 'input',
        parent: inputEntry,
        gen: result.gen,
      });
      head = inputEntry;
    }

    render();
  });
}

$(document).on('input', '.query-code', function() {
  var lines = $(this)
    .val()
    .split('\n').length;
  if ($(this).attr('rows') < lines) {
    $(this).attr('rows', lines);
  }
});

$(document).on('click', '.go-query', function() {
  handleSubmit($(this).parent());
});

$(document).on('click', '.edit-query', function() {
  var gen = $(this).data('gen');
  gen.parent.children.unshift({
    parent: gen.parent,
    gen: gen.result.parent,
    type: 'input',
    query: {
      code: $(this)
        .parent()
        .find('textarea')
        .text(),
    },
  });
  render();
});

$(document).on('keypress', '.query-code:not(.readonly)', function(ev) {
  if (ev.ctrlKey && ev.key === 'Enter') {
    $(this).blur();
    handleSubmit($(this).parent());
  }
});
