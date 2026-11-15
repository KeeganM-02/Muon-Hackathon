import { Routes, Route, Link, useParams } from 'react-router-dom';
import { Authenticator } from '@aws-amplify/ui-react';

function RoomPage() {
  const { roomId } = useParams();
  return (
    <div>
      <h2>Room {roomId}</h2>
      <p>Status: coming soon</p>
      <button>Claim</button> <button>Check in</button> <button>End</button>
    </div>
  );
}

function RoomsPage() {
  return (
    <div>
      <h2>Rooms</h2>
      <Link to="/r/1">Room 1</Link>
    </div>
  );
}

export default function App() {
  return (
    <Authenticator>
      {({ signOut, user }) => (
        <main style={{ maxWidth: 480, margin: '0 auto', padding: 16 }}>
          <p>
            Signed in as {user?.signInDetails?.loginId}{' '}
            <button onClick={signOut}>Sign out</button>
          </p>
          <Routes>
            <Route path="/" element={<RoomsPage />} />
            <Route path="/r/:roomId" element={<RoomPage />} />
          </Routes>
        </main>
      )}
    </Authenticator>
  );
}