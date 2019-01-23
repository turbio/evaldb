var dbname = window.location.hash.slice(1);

var gens = {
  0: {
    type: 'initial',
  },
};

var head = 0;

function query(req, cb) {
  $.ajax({
    url: '/eval/' + dbname,
    type: 'POST',
    data: JSON.stringify(req),
    contentType: 'application/json',
    success: cb,
  });
}

function ch(p) {
  if (p === undefined) {
    return [];
  }

  var c = [];
  var k = Object.keys(gens);
  for (var i = 0; i < k.length; i++) {
    if (gens[k[i]].parent === p) {
      c.push(gens[k[i]]);
    }
  }

  return c;
}

function ng(g) {
  var ng = {
    type: 'result',
  };

  Object.assign(ng, g.query);
  Object.assign(ng, g.result);

  if (ng.gen > head && ng.error === undefined) {
    head = ng.gen;
  }

  gens[ng.gen] = ng;
  return ng;
}

render();

function rdepth(gen, d) {
  if (d === undefined) {
    return rdepth(gen, 1);
  }

  var c = ch(gen.gen);

  var sum = d;
  for (var i = 0; i < c.length; i++) {
    sum += rdepth(c[i], d + 1);
  }

  return sum;
}

function inp(pgen, pnode) {
  var entry = $('<div class="entry"></div>');

  var tbox = $('<textarea class="query-code" rows="2"></textarea>');
  entry
    .addClass('query')
    .append(tbox)
    .append($('<button class="go-query">&nbsp;go&nbsp;</button>'))
    .data('gen', pgen);

  pnode.append(entry);

  if (head === pgen) {
    entry.addClass('athead');

    entry.find('textarea').focus();
    entry
      .find('textarea')
      .get(0)
      .scrollIntoView();
  }
}

function rtree(gid, pnode) {
  var entry = $('<div class="entry"></div>');

  var gen = gens[gid];

  if (gen.type === 'initial') {
    entry.addClass('initial-msg');
    entry.text('initial state');
  } else if (gen.type === 'result') {
    var error = gen.error;
    var object = gen.object;

    entry.append(
      $('<div class="query readonly"></div>').append(
        $('<textarea class="query-code" readonly="readonly"></textarea>').text(
          gen.code,
        ),
      ),
      //.append($('<button class="edit-query">edit</button>').data('gen', gid)),
    );

    entry.append(
      $('<div class="status-line"></div>').text(
        'gen: ' +
          gid +
          ' | ' +
          'pgen: ' +
          gen.parent +
          ' | ' +
          'walltime: ' +
          gen.walltime / 1e6 +
          'ms' +
          ' | ' +
          (gen.warm ? 'warm' : 'cold'),
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

  var c = ch(gid);

  for (var i = 0; i < c.length; i++) {
    if (i !== c.length - 1 || c[i].error !== undefined) {
      var gen = c[i];

      if (!gen.forceRender && rdepth(gen) > 1) {
        pnode.append(
          $('<div class="timeline fork"></div>').append(
            $('<div class="entry more">show more...</div>').on(
              'click',
              function(gen) {
                gen.forceRender = true;
                render();
              }.bind(null, gen),
            ),
          ),
        );
      } else {
        var fork = $('<div class="timeline fork"></div>');
        pnode.append(fork);
        rtree(c[i].gen, fork);
      }
    } else {
      rtree(c[i].gen, pnode);
    }
  }

  if (c.length === 0) {
    if (gen.error === undefined) {
      inp(gid, pnode);
    }
  } else {
    for (var i = 0; i < c.length; i++) {
      if (c[i].error === undefined) {
        return;
      }
    }

    inp(gid, pnode);
  }
}

function render() {
  $('#root-timeline').empty();
  rtree(0, $('#root-timeline'));
}

function handleSubmit(entry) {
  var q = entry.children('.query-code');
  var gen = entry.data('gen');

  var req = {
    code: q.val(),
    gen: gen === head ? undefined : gen,
  };

  query(req, function(result) {
    ng({ query: req, result: result });
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

  var genp = parent(gen);

  genp.children.unshift({
    gen: gen.parent,
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

var es = new EventSource('/tail/' + dbname);
es.addEventListener('transac', function(e) {
  const t = JSON.parse(e.data);
  ng(t);
  render();
});
