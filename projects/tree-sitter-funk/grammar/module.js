const {
  sep1,
  sep,
  parens,
  semi,
  semi_opt,
  semis,
} = require('./util.js')

module.exports = {

  // ------------------------------------------------------------------------
  // module names
  // ------------------------------------------------------------------------

  _modid: $ => alias($.name, $.module_id),

  _modid_prefix: $ => prec('qualifying-module', seq($._modid, $._any_tight_dot)),

  _qualifying_module: $ => repeat1($._modid_prefix),

  module: $ => seq(repeat($._modid_prefix), $._modid),

  // funk: a module path in a header, import or export may start lowercase, `graph`, `modules.listed`
  _path_id: $ => alias(choice($.name, $.variable), $.module_id),

  _lc_modid_prefix: $ => prec('qualifying-module', seq(alias($.variable, $.module_id), $._any_tight_dot)),

  _module_path_ids: $ => seq(repeat(seq($._path_id, $._any_tight_dot)), $._path_id),

  _module_path: $ => alias($._module_path_ids, $.module),

  // a qualifier starting lowercase, then Haskell's: `g`, `stack.Stack`
  _lc_type_module: $ => seq(repeat1($._lc_modid_prefix), $._qualifying_module),

  _lc_qualifying_module: $ => seq(repeat1($._lc_modid_prefix), optional($._qualifying_module)),

  // ------------------------------------------------------------------------
  // import/export
  // ------------------------------------------------------------------------

  namespace: _ => choice('pattern', 'type'),

  _child_type: $ => seq(field('namespace', 'type'), field('type', $._tyconids)),

  _child: $ => choice(
    alias($._child_type, $.associated_type),
    $._qname,
  ),

  children: $ => parens($, sep(',', field('element', choice(alias('..', $.all_names), $._child)))),

  _ie_entity: $ => seq(
    optional(field('namespace', $.namespace)),
    choice(
      field('variable', $._varids),
      field('type', $._tyconids),
      field('operator', choice($._sym_prefix, $._pqsym)),
    ),
    optional(field('children', $.children)),
  ),

  // ------------------------------------------------------------------------
  // import
  // ------------------------------------------------------------------------

  import_list: $ => parens(
    $,
    sep(',', field('name', alias($._ie_entity, $.import_name))),
    optional(','),
  ),

  import: $ => seq(
    'import',
    optional('qualified'),
    optional(field('package', alias($.string, $.import_package))),
    field('module', $._module_path),
    optional('qualified'),
    optional(seq('as', field('alias', $._module_path))),
    optional(seq(
      optional('hiding'),
      field('names', $.import_list),
    )),
  ),

  // ------------------------------------------------------------------------
  // export
  // ------------------------------------------------------------------------

  module_export: $ => seq('module', field('module', $._module_path)),

  exports: $ => parens(
    $,
    optional(sep1(',', choice(field('export', alias($._ie_entity, $.export)), $.module_export))),
    optional(','),
  ),

  // ------------------------------------------------------------------------
  // module body / sections
  // ------------------------------------------------------------------------

  header: $ => seq(
    'module',
    field('module', $._module_path),
    field('exports', optional($.exports)),
    $._where,
  ),

  imports: $ => seq(semis($, field('import', $.import)), semi($)),

  /**
   * Using `semi` at the end instead of `semi_opt` increases parser size by a full megabyte!!
   *
   * This allows imports after the first declaration to prevent the tree from jittering while typing an import:
   *
   * > import A
   * > imp
   * > import B
   *
   * The partially typed `imp` will be parsed as a `top_splice`, which forces `imports` to reduce after `import A`.
   * The rest of the file will then be part of `declarations` and all following imports will be broken until the keyword
   * has been completed.
   */
  declarations: $ => seq($.declaration, repeat(seq(semi($), choice($.declaration, $.import))), semi_opt($)),

  _body: $ => seq(
    choice($._cmd_layout_start, alias($._cmd_layout_start_explicit, '{')),
    semi_opt($),
    field('imports', optional($.imports)),
    field('declarations', optional($.declarations)),
    $._layout_end,
  ),

  _layout_end: $ => choice(
    $._cond_layout_end,
    alias($._cond_layout_end_explicit, '}'),
  ),

  /**
   * This is a supertype.
   */
  declaration: $ => choice(
    $.decl,
    $.type_synonym,
    $.kind_signature,
    $.type_family,
    $.type_instance,
    $.role_annotation,
    $.data_type,
    $.newtype,
    $.data_family,
    $.data_instance,
    $.class,
    $.instance,
    $.default_types,
    $.deriving_instance,
    $.pattern_synonym,
    $.foreign_import,
    $.foreign_export,
    $.foreign_library,
    $.foreign_struct,
    $.fixity,
    $.top_splice,
  ),

}
