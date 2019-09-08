'use strict';

const dbname = window.location.hash.slice(1);
$('.headbar > input').attr('value', dbname);

const e = React.createElement;

class Root extends React.Component {
  constructor(props) {
    super(props);
    this.state = {
      gens: {
        '0': { type: 'initial', children: [], id: 0 },
      },
      head: 0,
      newQuery: {
        args: [],
        code: '',
      },
    };
  }

  setQuery(newQuery) {
    this.setState({ newQuery: { ...this.state.newQuery, ...newQuery } });
  }

  mergeInTransaction(t) {
    const { gens, head } = this.state;

    const existing = gens[t.result.gen];

    if (existing) {
      t.children = existing.children;
    } else {
      t.children = [];
    }

    t.type = 'gen';
    t.id = t.result.gen;

    // fake a parent for a little while
    if (!gens[t.result.parent]) {
      gens[t.result.parent] = {
        result: { gen: t.result.parent },
        query: {},
        children: [],
        type: 'gen',
        id: t.result.parent,
      };
    }

    const parent = gens[t.result.parent];

    if (parent.children.filter(c => c.id === t.id).length === 0) {
      parent.children.push(t);
      parent.children.sort((l, r) => l.id - r.id);
    }

    this.setState({
      gens: {
        ...gens,
        [t.result.gen]: t,
      },
      head: head < t.id && !t.result.error ? t.id : head,
    });
  }

  doQuery() {
    const {
      head,
      newQuery: { code, args: aargs },
    } = this.state;

    const args = aargs.reduce((sum, c) => {
      if (!validJSON(c.value)) {
        sum[c.name] = 'invalid json!';
        return sum;
      }

      sum[c.name] = JSON.parse(c.value);

      return sum;
    }, {});

    $.ajax({
      url: '/eval/' + dbname,
      type: 'POST',
      data: JSON.stringify({
        code,
        args,
        gen: head,
      }),
      contentType: 'application/json',
      success: result => {
        this.mergeInTransaction({ query: { args, code, gen: head }, result });
      },
    });

    this.setQuery({ code: '', args: [] });
  }

  componentWillMount() {
    const es = new EventSource('/tail/' + dbname);
    es.addEventListener('transac', e => {
      const t = JSON.parse(e.data);
      this.mergeInTransaction(t);
    });
  }

  render() {
    const { gens, head, newQuery } = this.state;

    return e(Entry, {
      gen: gens[0],
      head,
      newQuery,
      setQuery: this.setQuery.bind(this),
      doQuery: this.doQuery.bind(this),
    });
  }
}

const Timeline = props =>
  e('div', { className: 'timeline fork' }, e(Entry, props));

const Entry = ({
  gen: { result, query, type, children, id },
  head,
  newQuery,
  setQuery,
  doQuery,
}) => [
  type === 'initial'
    ? e(InitialGen, { key: id })
    : e(Gen, { result, query, key: id }),
  ...children.map((ch, i, arr) =>
    e(i !== arr.length - 1 || id === head ? Timeline : Entry, {
      gen: ch,
      head,
      key: ch.id,
      newQuery,
      setQuery,
      doQuery,
    }),
  ),
  id === head
    ? e(Input, { key: 'input-' + head, newQuery, setQuery, doQuery })
    : undefined,
];

class Input extends React.Component {
  componentDidMount() {
    this.textarea && this.textarea.focus();
    this.entry && this.entry.scrollIntoView();
  }

  render() {
    return e(
      'div',
      {
        className: 'entry athead',
        ref: entry => (this.entry = entry),
      },
      e(
        'div',
        { className: 'query' },
        e(
          'div',
          {
            className: 'query-box',
          },
          e(EditArgs, {
            args: this.props.newQuery.args,
            setQuery: this.props.setQuery,
          }),
          e('textarea', {
            className: 'query-code',
            rows: 1,
            key: 'query',
            ref: textarea => {
              this.textarea = textarea;
            },
            value: this.props.newQuery.code,
            onChange: e => {
              if (!this.textarea) return;

              const lines = this.textarea.value.split('\n').length;

              if (this.textarea.attributes['rows'].value < lines) {
                this.textarea.attributes['rows'].value = lines;
              }

              this.props.setQuery({
                code: this.textarea.value,
              });
            },
            onKeyPress: e => {
              if (e.key === 'Enter' && e.ctrlKey) {
                this.props.doQuery();
              }
            },
          }),
          e('div', { className: 'func-head' }, 'end'),
        ),
        e(
          'button',
          {
            className: 'go-query',
            onClick: () => {
              this.props.doQuery();
            },
          },
          '\xA0go\xA0',
        ),
      ),
    );
  }
}

const validJSON = str => {
  try {
    JSON.parse(str);
  } catch (e) {
    return false;
  }

  return true;
};

// was previously:
//!!str.match(/^[a-zA-Z][a-zA-Z0-9]*$/);
// but actually we don't care that much
const validArg = str => true;

const Args = ({ args }) =>
  e(
    'div',
    { className: 'func-head' },
    'function (',
    Object.keys(args)
      .map(k => k + ' = ' + JSON.stringify(args[k]))
      .join(', '),
    ')',
  );

class ArgInput extends React.Component {
  componentDidMount() {
    this.nameInput && this.nameInput.focus();
  }

  render() {
    const { a, i, arr, setQuery } = this.props;

    return e(
      'span',
      { key: i },
      e('input', {
        type: 'text',
        key: 'key-' + i,
        placeholder: 'name',
        ref: r => (this.nameInput = r),
        className: 'arg' + (!validArg(a.name) ? ' err' : ''),
        size: a.name.length > 4 ? a.name.length : 4,
        value: a.name,
        onChange: e => {
          setQuery({
            args: [
              ...arr.slice(0, i),
              { ...a, name: e.target.value },
              ...arr.slice(i + 1),
            ],
          });
        },
      }),
      ' = ',
      e('input', {
        type: 'text',
        key: 'value-' + i,
        placeholder: 'json',
        className: 'arg' + (!validJSON(a.value) ? ' err' : ''),
        size: a.value.length > 4 ? a.value.length : 4,
        value: a.value,
        onChange: e =>
          e.target.value.slice(-1) === ','
            ? setQuery({ args: [...arr, { name: '', value: '' }] })
            : setQuery({
                args: [
                  ...arr.slice(0, i),
                  { ...a, value: e.target.value },
                  ...arr.slice(i + 1),
                ],
              }),
      }),
      i !== arr.length - 1 ? ', ' : undefined,
    );
  }
}

const EditArgs = ({ args, setQuery }) =>
  e(
    'div',
    { className: 'func-head' },
    'function (',
    args.map((a, i, arr) => e(ArgInput, { key: i, a, i, arr, setQuery })),
    e(
      'button',
      {
        className: 'arg',
        onClick: () => setQuery({ args: [...args, { name: '', value: '' }] }),
      },
      '+',
    ),
    ')',
  );

const InitialGen = () =>
  e('div', { className: 'entry initial-msg' }, 'initial state');

const Gen = ({
  result: { gen, parent, walltime, warn, error, object, warm },
  query: { code, args },
}) =>
  e(
    'div',
    { className: 'entry' + (error ? ' error' : '') },
    e(
      'div',
      { className: 'query readonly' },
      e(
        'div',
        { className: 'query-box' },
        e(Args, { args }),
        e('textarea', {
          className: 'query-code',
          readOnly: true,
          value: code,
          rows: Math.min(10, code.split('\n').length),
        }),
        e('div', { className: 'func-head' }, 'end'),
      ),
    ),
    e(
      'div',
      { className: 'status-line' },
      'gen: ' +
        gen +
        ' | ' +
        'pgen: ' +
        parent +
        ' | ' +
        'walltime: ' +
        walltime / 1e6 +
        'ms' +
        ' | ' +
        (warm ? 'warm' : 'cold'),
      e(
        'span',
        { style: { float: 'right' } },
        'as: ',
        // <input type="radio" name="gender" value="male"> Male<br>
        // <input type="radio" name="gender" value="female"> Female<br>
        e(
          'label',
          {
            for: 'cURL',
            className: 'as-code-label',
          },
          'cURL',
        ),
        e('input', {
          className: 'as-code-radio',
          id: 'cURL',
          type: 'radio',
          name: 'as-code',
          value: 'cURL',
        }),
        e(
          'div',
          {
            className: 'as-code-box',
          },
          "curl '" +
            window.location.host +
            '/' +
            dbname +
            "/eval' " +
            "-H 'Content-Type: application/json' " +
            "--data '" +
            JSON.stringify({
              code,
              args: Object.keys(args).length ? args : undefined,
            }) +
            "'",
        ),
        e('input', {
          className: 'as-code-radio',
          type: 'radio',
          name: 'as-code',
          value: 'none',
        }),
      ),
    ),
    error
      ? e('div', { className: 'result error' }, JSON.stringify(error, null, 2))
      : e('div', { className: 'result' }, JSON.stringify(object, null, 2)),
  );

const dom = document.querySelector('#timeline');
ReactDOM.render(e(Root), dom);
