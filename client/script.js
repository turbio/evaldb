function query(code, cb) {
  $.post('/eval', { code }, function(data, status) {
    cb(data);
  });
}

function handleSubmit(entry) {
  var btn = entry.children('.go-query');
  var q = entry.children('.query-code');

  btn.addClass('readonly');
  entry.addClass('readonly');
  q.addClass('readonly');

  q.attr('readonly', true);
  query(q.val(), function(result) {
    var c;
    if (result.error) {
      var err = $('<div class="entry error"></div>').text(result.error);
      c = $('<div class="timeline fork"></div>').append(err);
    } else {
      c = $('<div class="entry result"></div>').text(
        '= ' + JSON.stringify(result.object, null, 2),
      );
    }

    entry
      .parent()
      .append(
        $('<div class="entry"></div>').text(result.walltime / 1000000 + 'ms'),
      );
    entry
      .parent()
      .append(
        $('<div class="entry"></div>').text(result.warm ? 'warm' : 'cold'),
      );
    entry
      .parent()
      .append($('<div class="entry"></div>').text('seq: ' + result.seq));
    entry.parent().append(c);

    var newEntry = $(
      '<div class="entry query">' +
        '<textarea class="query-code" rows="2"></textarea>' +
        '<button class="go-query">go</button>' +
        '</div>',
    );
    entry.parent().append(newEntry);
    newEntry.children('.query-code').focus();
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

$(document).on('click', '.go-query:not(.readonly)', function() {
  handleSubmit($(this).parent());
});

$(document).on('keypress', '.query-code:not(.readonly)', function(ev) {
  if (ev.ctrlKey && ev.key === 'Enter') {
    $(this).blur();
    handleSubmit($(this).parent());
  }
});
