import type * as Preset from '@docusaurus/preset-classic'
import type { Config } from '@docusaurus/types'
import { themes as prismThemes } from 'prism-react-renderer'

const organization = 'sajonaro'
const project = 'pg_describe'
const repo = `https://github.com/${organization}/${project}`

/**
 * Docs-only mode: the documentation is the site. Pages live in ../docs as
 * plain Markdown so they render on GitHub too, and this project holds only
 * the presentation.
 */
const config: Config = {
  title: 'pg_describe',
  tagline: 'Ask PostgreSQL what a query would return, without running it',
  favicon: 'img/favicon.svg',

  url: `https://${organization}.github.io`,
  baseUrl: `/${project}/`,
  organizationName: organization,
  projectName: project,
  trailingSlash: false,

  // A link to a page that was renamed or never written should fail the build,
  // not ship a 404.
  onBrokenLinks: 'throw',
  onBrokenAnchors: 'throw',
  markdown: {
    hooks: {
      onBrokenMarkdownLinks: 'throw',
    },
  },

  i18n: {
    defaultLocale: 'en',
    locales: ['en'],
  },

  presets: [
    [
      'classic',
      {
        docs: {
          path: '../docs',
          routeBasePath: '/',
          sidebarPath: './sidebars.ts',
          editUrl: `${repo}/edit/main/`,
          showLastUpdateTime: true,
        },
        blog: false,
        theme: {
          customCss: './src/css/custom.css',
        },
      } satisfies Preset.Options,
    ],
  ],

  themeConfig: {
    colorMode: {
      respectPrefersColorScheme: true,
    },
    navbar: {
      title: 'pg_describe',
      items: [
        {
          type: 'docSidebar',
          sidebarId: 'docs',
          position: 'left',
          label: 'Documentation',
        },
        {
          to: '/end-to-end-example',
          label: 'Example',
          position: 'left',
        },
        {
          href: 'https://pgxn.org/dist/pg_describe/',
          label: 'PGXN',
          position: 'right',
        },
        {
          href: 'https://www.npmjs.com/package/pg-describe-gen',
          label: 'npm',
          position: 'right',
        },
        {
          href: repo,
          label: 'GitHub',
          position: 'right',
        },
      ],
    },
    footer: {
      style: 'dark',
      links: [
        {
          title: 'Documentation',
          items: [
            { label: 'Overview', to: '/' },
            { label: 'Getting started', to: '/getting-started' },
            { label: 'End-to-end example', to: '/end-to-end-example' },
            { label: 'Function reference', to: '/function-reference' },
          ],
        },
        {
          title: 'Project',
          items: [
            { label: 'GitHub', href: repo },
            { label: 'Issues', href: `${repo}/issues` },
            { label: 'Contributing', to: '/contributing' },
          ],
        },
        {
          title: 'Prior art',
          items: [
            { label: 'pgTyped', href: 'https://github.com/adelsz/pgtyped' },
            { label: 'sqlc', href: 'https://sqlc.dev' },
            { label: 'sqlx', href: 'https://github.com/launchbadge/sqlx' },
          ],
        },
      ],
      copyright: `MIT licensed. Built with Docusaurus.`,
    },
    prism: {
      theme: prismThemes.github,
      darkTheme: prismThemes.dracula,
      additionalLanguages: ['sql', 'bash', 'json', 'c'],
    },
  } satisfies Preset.ThemeConfig,
}

export default config
