// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Vista "Mi cuenta": cambio de contraseña. Sin enter()/exit(): la cabecera de
// cuenta ya la mantiene al día setUser() (shared.js) en cada login.
import { $, api, showLogin } from '../shared.js';
import { t } from '../i18n.js';

$('#password-form').addEventListener('submit', async (event) => {
  event.preventDefault();
  const button = $('#password-button');
  $('#password-error').textContent = '';
  $('#password-success').textContent = '';
  button.disabled = true;
  try {
    await api('/api/account/password', { method: 'POST', body: JSON.stringify({ currentPassword: $('#current-password').value, newPassword: $('#new-password').value }) });
    $('#password-success').textContent = t('Contraseña cambiada.');
    event.target.reset();
  } catch (error) {
    if (error.status === 401 && error.message !== t('La contraseña actual no es correcta')) return showLogin();
    $('#password-error').textContent = error.message;
  } finally {
    button.disabled = false;
  }
});
