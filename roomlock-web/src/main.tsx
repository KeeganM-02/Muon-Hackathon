import React from 'react';
import ReactDOM from 'react-dom/client';
import { BrowserRouter } from 'react-router-dom';
import { Amplify } from 'aws-amplify';
import '@aws-amplify/ui-react/styles.css';
import App from './App';

Amplify.configure({
  Auth: {
    Cognito: {
      userPoolId: 'us-east-2_SDmtOUvtp',
      userPoolClientId: '5bo2ki57dkj6djpvr3t86cgt80',
      loginWith: { email: true },
    },
  },
});

ReactDOM.createRoot(document.getElementById('root')!).render(
  <React.StrictMode>
    <BrowserRouter>
      <App />
    </BrowserRouter>
  </React.StrictMode>
);