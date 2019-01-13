var dbname = window.location.hash.slice(1);

function query(code, cb) {
  $.ajax({
    url: '/eval/' + dbname,
    type: 'POST',
    data: JSON.stringify({ code }),
    contentType: 'application/json',
    success: cb,
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
    var resultEntry;
    if (result.error) {
      var resultEntry = $('<div class="entry error"></div>').text(
        JSON.stringify(result.error, null, 2),
      );
      entry
        .parent()
        .append($('<div class="timeline fork"></div>').append(resultEntry));
    } else {
      resultEntry = $('<div class="entry result"></div>').text(
        '= ' + JSON.stringify(result.object, null, 2),
      );
      entry.parent().append(resultEntry);
    }

    var statusLine = $('<div class="status-line"></div>');
    statusLine.text(
      result.walltime / 1e6 + 'ms' + ' | ' + (result.warm ? 'warm' : 'cold'),
    );

    resultEntry.append(statusLine);

    var newEntry = $(
      '<div class="entry query">' +
        '<textarea data-gramm_editor="false" class="query-code" rows="2"></textarea>' +
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
